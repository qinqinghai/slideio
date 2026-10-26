#pragma once

#include "slideio/core/cvscene.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace slideio {
namespace incell {

struct InCellPlane {
    std::string reference;
    std::filesystem::path path;
    double timestamp_sec = 0.0;
};

struct InCellChannel {
    std::string name;
    double emission_nm = 0.0;
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
