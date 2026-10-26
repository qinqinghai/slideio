#pragma once

#include "slideio/core/cvscene.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace slideio {
namespace incell {

// Per-image statistics. Every plane has its own copy: the values change each frame.
struct InCellPlaneStats {
    bool present = false;
    double min_value = 0.0;
    double max_value = 0.0;
    double mean = 0.0;
    double sbr = 0.0;
};

// Stage pose for one image. Planes that carry the same numbers share one instance.
struct InCellPlaneGeometry {
    bool has_offset = false;
    double offset_x = 0.0;
    double offset_y = 0.0;
    std::string offset_unit;
    bool has_focus = false;
    double focus_z = 0.0;
    std::string focus_unit;
    bool has_plate = false;
    double plate_x = 0.0;
    double plate_y = 0.0;

    bool operator==(const InCellPlaneGeometry& other) const {
        return has_offset == other.has_offset && offset_x == other.offset_x &&
               offset_y == other.offset_y && offset_unit == other.offset_unit &&
               has_focus == other.has_focus && focus_z == other.focus_z &&
               focus_unit == other.focus_unit && has_plate == other.has_plate &&
               plate_x == other.plate_x && plate_y == other.plate_y;
    }
};

// Exposure recorded on the Image element. Shared when the numbers match.
struct InCellPlaneExposure {
    double time = 0.0;
    std::string unit;

    bool operator==(const InCellPlaneExposure& other) const {
        return time == other.time && unit == other.unit;
    }
};

struct InCellPlane {
    std::string reference;
    std::filesystem::path path;
    double timestamp_sec = 0.0;
    // Image@acquisition_time_ms: per-plane exposure duration on IN Cell 2200,
    // not the acquisition clock (that is timestamp_sec).
    double acquisition_time_ms = 0.0;
    InCellPlaneStats stats;
    std::shared_ptr<const InCellPlaneGeometry> geometry;
    std::shared_ptr<const InCellPlaneExposure> exposure;
};

struct InCellChannel {
    std::string name;
    double emission_nm = 0.0;
    double excitation_nm = 0.0;
    std::string emission_filter;
    std::string excitation_filter;
    double exposure_ms = 0.0;
    std::string exposure_unit;
    std::string polychroic;
    std::string imaging_mode;
};

struct InCellSceneData {
    std::string file_path;
    std::string name;
    int scene_index = 0;
    int width = 0;
    int height = 0;
    int num_t = 0;
    int num_c = 0;
    int num_z = 0;
    DataType data_type = DataType::DT_Unknown;
    Compression compression = Compression::Unknown;
    double magnification = 0.0;
    Resolution resolution;
    double frame_interval_s = 0.0;
    int64_t acquisition_time = 0;
    std::vector<InCellChannel> channels;
    std::vector<InCellPlane> planes;
};

class InCellScene : public CVScene {
public:
    explicit InCellScene(InCellSceneData data);

    std::string getFilePath() const override;
    int getSceneIndex() const override;
    const std::string& getDriverId() const override;
    std::string getName() const override;
    cv::Rect getRect() const override;
    int getNumChannels() const override;
    int getNumZSlices() const override;
    int getNumTFrames() const override;
    DataType getChannelDataType(int channel) const override;
    std::string getChannelName(int channel) const override;
    Resolution getResolution() const override;
    double getTFrameResolution() const override;
    bool hasPlaneTimestamps() const override;
    double getPlaneTimestamp(int tFrame, int channel, int zSlice) const override;
    int64_t getAcquisitionTime() const override;
    double getMagnification() const override;
    Compression getCompression() const override;
    void readResampledBlockChannelsEx(const cv::Rect& blockRect, const cv::Size& blockSize,
                                      const std::vector<int>& componentIndices, int zSliceIndex,
                                      int tFrameIndex, cv::OutputArray output) override;

private:
    const InCellPlane& planeAt(int t, int c, int z) const;
    InCellSceneData data_;
    std::string driver_id_;
};

}  // namespace incell
}  // namespace slideio
