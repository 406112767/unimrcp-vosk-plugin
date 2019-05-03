#include "kaldi_recognizer.h"

#include "fstext/fstext-utils.h"
#include "lat/sausages.h"

using namespace fst;
using namespace kaldi::nnet3;

KaldiRecognizer::KaldiRecognizer(Model &model) : model_(model) {

    feature_info_ = new kaldi::OnlineNnet2FeaturePipelineInfo(model_.feature_config_);
    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (*feature_info_);
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_.trans_model_, feature_info_->silence_weighting_config, 3);

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_.nnet3_decoding_config_,
            *model_.trans_model_,
            *model_.decodable_info_,
            *model_.decode_fst_,
            feature_pipeline_);

    input_finalized_ = false;
}

KaldiRecognizer::~KaldiRecognizer() {
    delete feature_pipeline_;
    delete feature_info_;
    delete silence_weighting_;

    delete decoder_;
}

void KaldiRecognizer::CleanUp()
{
    delete decoder_;

    OnlineIvectorExtractorAdaptationState state(feature_info_->ivector_extractor_info);
    feature_pipeline_->GetAdaptationState(&state);

    delete feature_pipeline_;

    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (*feature_info_);
    feature_pipeline_->SetAdaptationState(state);

    delete silence_weighting_;
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_.trans_model_, feature_info_->silence_weighting_config, 3);

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_.nnet3_decoding_config_,
            *model_.trans_model_,
            *model_.decodable_info_,
            *model_.decode_fst_,
            feature_pipeline_);
}

bool KaldiRecognizer::AcceptWaveform(const char *data, int len) {

    if (input_finalized_) {
        CleanUp();
        input_finalized_ = false;
    }

    Vector<BaseFloat> wave;
    wave.Resize(len / 2, kUndefined);
    for (int i = 0; i < len / 2; i++)
        wave(i) = *(((short *)data) + i);

    feature_pipeline_->AcceptWaveform(8000, wave);

    if (silence_weighting_->Active() && feature_pipeline_->NumFramesReady() > 0 &&
        feature_pipeline_->IvectorFeature() != NULL) {
        std::vector<std::pair<int32, BaseFloat> > delta_weights;
        silence_weighting_->ComputeCurrentTraceback(decoder_->Decoder());
        silence_weighting_->GetDeltaWeights(feature_pipeline_->NumFramesReady(),
                                          &delta_weights);
        feature_pipeline_->IvectorFeature()->UpdateFrameWeights(delta_weights);
    }

    decoder_->AdvanceDecoding();

    if (decoder_->EndpointDetected(model_.endpoint_config_)) {
        return true;
    }

    return false;
}

std::string KaldiRecognizer::Result()
{

    if (!input_finalized_) {
        feature_pipeline_->InputFinished();

        if (silence_weighting_->Active() && feature_pipeline_->NumFramesReady() > 0 &&
            feature_pipeline_->IvectorFeature() != NULL) {
            std::vector<std::pair<int32, BaseFloat> > delta_weights;
            silence_weighting_->ComputeCurrentTraceback(decoder_->Decoder());
            silence_weighting_->GetDeltaWeights(feature_pipeline_->NumFramesReady(),
                                              &delta_weights);
            feature_pipeline_->IvectorFeature()->UpdateFrameWeights(delta_weights);
        }
        decoder_->AdvanceDecoding();
        decoder_->FinalizeDecoding();

        input_finalized_ = true;
    }

    kaldi::CompactLattice clat;
    decoder_->GetLattice(true, &clat);

    CompactLattice aligned_lat;
    WordAlignLattice(clat, *model_.trans_model_, *model_.winfo_, 0, &aligned_lat);
    MinimumBayesRisk mbr(aligned_lat);

    const std::vector<BaseFloat> &conf = mbr.GetOneBestConfidences();
    const std::vector<int32> &words = mbr.GetOneBest();
    const std::vector<std::pair<BaseFloat, BaseFloat> > &times =
          mbr.GetOneBestTimes();

    int size = words.size();

    std::stringstream ss;

    // Create JSON object
    ss << "{\"result\" : [ ";
    for (int i = 0; i < size; i++) {
        ss << "{\"word\": \"" << model_.word_syms_->Find(words[i]) << "\", \"start\" : " << times[i].first << "," <<
                " \"end\" : " << times[i].second << ", \"conf\" : " << conf[i] << "}";
        if (i != size - 1)
            ss << ",\n";
        else
            ss << "\n";
    }
    ss << " ], \"text\" : \"";
    for (int i = 0; i < size; i++) {
        ss << model_.word_syms_->Find(words[i]);
        if (i != size - 1)
            ss << " ";
    }
    ss << "\" }";

    return ss.str();
}

std::string KaldiRecognizer::PartialResult()
{
    decoder_->AdvanceDecoding();
    if (decoder_->NumFramesDecoded() < 50) {
        return "{\"partial\" : \"\"}";
    }

    kaldi::Lattice lat;
    decoder_->GetBestPath(false, &lat);
    std::vector<kaldi::int32> alignment, words;
    LatticeWeight weight;
    GetLinearSymbolSequence(lat, &alignment, &words, &weight);

    std::ostringstream outss;
    outss << "{\"partial\" : \"";
    for (size_t i = 0; i < words.size(); i++) {
        if (i) {
            outss << " ";
        }
        outss << model_.word_syms_->Find(words[i]);
    }
    outss << "\"}";

    return outss.str();
}
