#include "BoTSORT.h"
#include "matching.h"

BoTSORT::BoTSORT(
        std::optional<const char *> model_weights,
        bool fp16_inference,
        float track_high_thresh,
        float new_track_thresh,
        uint8_t track_buffer,
        float match_thresh,
        float proximity_thresh,
        float appearance_thresh,
        const char *gmc_method,
        uint8_t frame_rate,
        float lambda)
    : _track_high_thresh(track_high_thresh),
      _new_track_thresh(new_track_thresh),
      _track_buffer(track_buffer),
      _match_thresh(match_thresh),
      _proximity_thresh(proximity_thresh),
      _appearance_thresh(appearance_thresh),
      _frame_rate(frame_rate),
      _lambda(lambda) {

    // Tracker module
    _frame_id = 0;
    _buffer_size = static_cast<uint8_t>(_frame_rate / 30.0 * _track_buffer);
    _max_time_lost = _buffer_size;
    _kalman_filter = std::make_shared<KalmanFilter>(static_cast<double>(1.0 / _frame_rate));


    // Re-ID module, load visual feature extractor here
    if (model_weights.has_value()) {
        _reid_model = std::make_unique<ReIDModel>(model_weights.value(), fp16_inference);
        _reid_enabled = true;
    } else {
        std::cout << "Re-ID module disabled" << std::endl;
        _reid_enabled = false;
    }


    // Global motion compensation module
    _gmc_algo = std::make_unique<GlobalMotionCompensation>(GMC_method_map[gmc_method]);
}

std::vector<Track> BoTSORT::track(const std::vector<Detection> &detections, const cv::Mat &frame) {
    ////////////////// CREATE TRACK OBJECT FOR ALL THE DETECTIONS //////////////////
    // For all detections, extract features, create tracks and classify on the segregate of confidence
    _frame_id++;
    std::vector<Track *> detections_high_conf, detections_low_conf;
    std::vector<Track *> activated_tracks, refind_tracks;

    if (detections.size() > 0) {
        for (Detection &detection: const_cast<std::vector<Detection> &>(detections)) {
            detection.bbox_tlwh.x = std::max(0.0f, detection.bbox_tlwh.x);
            detection.bbox_tlwh.y = std::max(0.0f, detection.bbox_tlwh.y);
            detection.bbox_tlwh.width = std::min(static_cast<float>(frame.cols - 1), detection.bbox_tlwh.width);
            detection.bbox_tlwh.height = std::min(static_cast<float>(frame.rows - 1), detection.bbox_tlwh.height);

            Track *tracklet;
            std::vector<float> tlwh = {detection.bbox_tlwh.x, detection.bbox_tlwh.y, detection.bbox_tlwh.width, detection.bbox_tlwh.height};
            if (_reid_enabled) {
                FeatureVector embedding = _extract_features(frame, detection.bbox_tlwh);
                tracklet = new Track(tlwh, detection.confidence, detection.class_id, embedding);
            } else {
                tracklet = new Track(tlwh, detection.confidence, detection.class_id);
            }

            if (detection.confidence >= _track_high_thresh) {
                detections_high_conf.push_back(tracklet);
            } else if (detection.confidence > 0.1 && detection.confidence < _track_high_thresh) {
                detections_low_conf.push_back(tracklet);
            }
        }
    }

    // Segregate tracks in unconfirmed and tracked tracks
    std::vector<Track *> unconfirmed_tracks, tracked_tracks;
    for (Track *track: _tracked_tracks) {
        if (!track->is_activated) {
            unconfirmed_tracks.push_back(track);
        } else {
            tracked_tracks.push_back(track);
        }
    }
    ////////////////// CREATE TRACK OBJECT FOR ALL THE DETECTIONS //////////////////


    ////////////////// Apply KF predict and GMC before running assocition algorithm //////////////////
    // Merge currently tracked tracks and lost tracks
    std::vector<Track *> tracks_pool;
    tracks_pool = _merge_track_lists(tracked_tracks, _lost_tracks);

    // Predict the location of the tracks with KF (even for lost tracks)
    Track::multi_predict(tracks_pool, *_kalman_filter);

    // Estimate camera motion and apply camera motion compensation
    HomographyMatrix H = _gmc_algo->apply(frame, detections);
    Track::multi_gmc(tracks_pool, H);
    Track::multi_gmc(unconfirmed_tracks, H);
    ////////////////// Apply KF predict and GMC before running assocition algorithm //////////////////


    ////////////////// ASSOCIATION ALGORITHM STARTS HERE //////////////////

    ////////////////// First association, with high score detection boxes //////////////////
    CostMatrix iou_dists, raw_emd_dist;

    // Find IoU distance between all tracked tracks and high confidence detections
    iou_dists = iou_distance(tracks_pool, detections_high_conf);
    fuse_score(iou_dists, detections_high_conf);// Fuse the score with IoU distance

    if (_reid_enabled) {
        // If re-ID is enabled, find the embedding distance between all tracked tracks and high confidence detections
        raw_emd_dist = embedding_distance(tracks_pool, detections_high_conf);
        fuse_motion(*_kalman_filter, raw_emd_dist, tracks_pool, detections_high_conf, false, _lambda);// Fuse the motion with embedding distance
    }

    // Fuse the IoU distance and embedding distance to get the final distance matrix
    CostMatrix distances_first_association = fuse_iou_with_emb(iou_dists, raw_emd_dist, _proximity_thresh, _appearance_thresh);

    // Perform linear assignment on the final distance matrix, LAPJV algorithm is used here
    AssociationData first_associations;
    linear_assignment(distances_first_association, _match_thresh, first_associations);

    // Update the tracks with the associated detections
    for (size_t i = 0; i < first_associations.matches.size(); i++) {
        Track *track = tracks_pool[first_associations.matches[i].first];
        Track *detection = detections_high_conf[first_associations.matches[i].second];

        // If track was being actively tracked, we update the track with the new associated detection
        if (track->state == TrackState::Tracked) {
            track->update(*_kalman_filter, *detection, _frame_id);
            activated_tracks.push_back(track);
        } else {
            // If track was not being actively tracked, we re-activate the track with the new associated detection
            // NOTE: There should be a minimum number of frames before a track is re-activated
            track->re_activate(*_kalman_filter, *detection, _frame_id, false);
            refind_tracks.push_back(track);
        }
    }
    ////////////////// First association, with high score detection boxes //////////////////


    ////////////////// Second association, with low score detection boxes //////////////////
    // Get all unmatched but tracked tracks after the first association, these tracks will be used for the second association
    std::vector<Track *> unmatched_tracks_after_1st_association;
    for (size_t i = 0; i < first_associations.unmatched_track_indices.size(); i++) {
        int track_idx = first_associations.unmatched_track_indices[i];
        Track *track = tracks_pool[track_idx];
        if (track->state == TrackState::Tracked) {
            unmatched_tracks_after_1st_association.push_back(track);
        }
    }

    // Find IoU distance between unmatched but tracked tracks left after the first association and low confidence detections
    CostMatrix iou_dists_second;
    iou_distance(unmatched_tracks_after_1st_association, detections_low_conf);

    // Perform linear assignment on the final distance matrix, LAPJV algorithm is used here
    AssociationData second_associations;
    linear_assignment(iou_dists_second, 0.5, second_associations);

    // Update the tracks with the associated detections
    for (size_t i = 0; i < second_associations.matches.size(); i++) {
        Track *track = unmatched_tracks_after_1st_association[second_associations.matches[i].first];
        Track *detection = detections_low_conf[second_associations.matches[i].second];

        // If track was being actively tracked, we update the track with the new associated detection
        if (track->state == TrackState::Tracked) {
            track->update(*_kalman_filter, *detection, _frame_id);
            activated_tracks.push_back(track);
        } else {
            // If track was not being actively tracked, we re-activate the track with the new associated detection
            // NOTE: There should be a minimum number of frames before a track is re-activated
            track->re_activate(*_kalman_filter, *detection, _frame_id, false);
            refind_tracks.push_back(track);
        }
    }

    // The tracks that are not associated with any detection even after the second association are marked as lost
    std::vector<Track *> lost_tracks;
    for (size_t i = 0; i < second_associations.unmatched_track_indices.size(); i++) {
        Track *track = unmatched_tracks_after_1st_association[second_associations.unmatched_track_indices[i]];
        if (track->state != TrackState::Lost) {
            track->mark_lost();
            lost_tracks.push_back(track);
        }
    }
    ////////////////// Second association, with low score detection boxes //////////////////


    ////////////////// Deal with unconfirmed tracks //////////////////
    ////////////////// Deal with unconfirmed tracks //////////////////


    // Added for code compilation
    return std::vector<Track>();
}

FeatureVector BoTSORT::_extract_features(const cv::Mat &frame, const cv::Rect_<float> &bbox_tlwh) {
    cv::Mat patch = frame(bbox_tlwh);
    cv::Mat patch_resized;
    return _reid_model->extract_features(patch_resized);
}

std::vector<Track *> BoTSORT::_merge_track_lists(std::vector<Track *> &tracks_list_a, std::vector<Track *> &tracks_list_b) {
    std::map<int, bool> exists;
    std::vector<Track *> merged_tracks_list;

    for (Track *track: tracks_list_a) {
        exists[track->track_id] = true;
        merged_tracks_list.push_back(track);
    }

    for (Track *track: tracks_list_b) {
        if (exists.find(track->track_id) == exists.end()) {
            exists[track->track_id] = true;
            merged_tracks_list.push_back(track);
        }
    }

    return merged_tracks_list;
}
