#include "slideio/drivers/incell/incellscene.hpp"

#include "slideio/core/exceptions.hpp"
#include "slideio/core/tools/tools.hpp"
#include "slideio/imagetools/tifftools.hpp"

#include <cmath>

using namespace slideio;
using namespace slideio::incell;

namespace {

[[noreturn]] void failRead(const InCellSceneData& data, int t, int c, int z, const std::string& why) {
    const int index = (t * data.num_c + c) * data.num_z + z;
    const InCellPlane* plane = (index >= 0 && index < static_cast<int>(data.planes.size()))
                                   ? &data.planes[static_cast<std::size_t>(index)]
                                   : nullptr;
    RAISE_RUNTIME_ERROR << "INCELL read failed scene='" << data.name << "' t=" << t << " c=" << c
                        << " z=" << z
                        << " file='" << (plane ? plane->reference : std::string()) << "' path='"
                        << (plane ? plane->path.string() : std::string()) << "': " << why;
}

}  // namespace

InCellScene::InCellScene(InCellSceneData data) : data_(std::move(data)), driver_id_("INCELL") {
    for (int c = 0; c < data_.num_c; ++c) {
        const auto& channel = data_.channels[static_cast<std::size_t>(c)];
        if (channel.emission_nm > 0.0) {
            setChannelAttribute(c, "EmissionWavelength", channel.emission_nm);
        }
        if (channel.excitation_nm > 0.0) {
            setChannelAttribute(c, "ExcitationWavelength", channel.excitation_nm);
        }
    }
}

std::string InCellScene::getFilePath() const { return data_.file_path; }

int InCellScene::getSceneIndex() const { return data_.scene_index; }

const std::string& InCellScene::getDriverId() const { return driver_id_; }

std::string InCellScene::getName() const { return data_.name; }

cv::Rect InCellScene::getRect() const { return cv::Rect(0, 0, data_.width, data_.height); }

int InCellScene::getNumChannels() const { return data_.num_c; }

int InCellScene::getNumZSlices() const { return data_.num_z; }

int InCellScene::getNumTFrames() const { return data_.num_t; }

DataType InCellScene::getChannelDataType(int channel) const {
    if (channel < 0 || channel >= data_.num_c) {
        RAISE_RUNTIME_ERROR << "INCELL channel out of range: " << channel;
    }
    return data_.data_type;
}

std::string InCellScene::getChannelName(int channel) const {
    if (channel < 0 || channel >= data_.num_c) {
        RAISE_RUNTIME_ERROR << "INCELL channel out of range: " << channel;
    }
    return data_.channels[static_cast<std::size_t>(channel)].name;
}

Resolution InCellScene::getResolution() const { return data_.resolution; }

double InCellScene::getTFrameResolution() const { return data_.frame_interval_s; }

bool InCellScene::hasPlaneTimestamps() const { return data_.num_t * data_.num_c * data_.num_z > 0; }

double InCellScene::getPlaneTimestamp(int tFrame, int channel, int zSlice) const {
    if (!hasPlaneTimestamps() || tFrame < 0 || channel < 0 || zSlice < 0 || tFrame >= data_.num_t ||
        channel >= data_.num_c || zSlice >= data_.num_z) {
        return 0.0;
    }
    const double origin = static_cast<double>(data_.acquisition_time);
    return planeAt(tFrame, channel, zSlice).timestamp_sec - origin;
}

int64_t InCellScene::getAcquisitionTime() const { return data_.acquisition_time; }

double InCellScene::getMagnification() const { return data_.magnification; }

Compression InCellScene::getCompression() const { return data_.compression; }

const InCellPlane& InCellScene::planeAt(int t, int c, int z) const {
    const int index = (t * data_.num_c + c) * data_.num_z + z;
    if (index < 0 || index >= static_cast<int>(data_.planes.size())) {
        failRead(data_, t, c, z, "plane is not in the dense grid");
    }
    return data_.planes[static_cast<std::size_t>(index)];
}

void InCellScene::readResampledBlockChannelsEx(const cv::Rect& blockRect, const cv::Size& blockSize,
                                               const std::vector<int>& componentIndices,
                                               int zSliceIndex, int tFrameIndex,
                                               cv::OutputArray output) {
    if (tFrameIndex < 0 || tFrameIndex >= data_.num_t || zSliceIndex < 0 ||
        zSliceIndex >= data_.num_z) {
        failRead(data_, tFrameIndex, 0, zSliceIndex, "plane index out of range");
    }
    if (blockSize.width <= 0 || blockSize.height <= 0 || blockRect.width <= 0 ||
        blockRect.height <= 0) {
        RAISE_RUNTIME_ERROR << "INCELL read requested an empty block";
    }
    const cv::Rect image(0, 0, data_.width, data_.height);
    if ((blockRect & image) != blockRect) {
        failRead(data_, tFrameIndex, 0, zSliceIndex, "block rectangle is outside the image");
    }

    std::vector<int> channels = componentIndices;
    if (channels.empty()) {
        channels.resize(static_cast<std::size_t>(data_.num_c));
        for (int c = 0; c < data_.num_c; ++c) {
            channels[static_cast<std::size_t>(c)] = c;
        }
    }

    std::vector<cv::Mat> parts;
    parts.reserve(channels.size());
    for (int channel : channels) {
        if (channel < 0 || channel >= data_.num_c) {
            failRead(data_, tFrameIndex, channel, zSliceIndex, "channel index out of range");
        }
        const InCellPlane& plane = planeAt(tFrameIndex, channel, zSliceIndex);
        cv::Mat raster;
        try {
            TiffTools::readDirRaster(plane.path.string(), 0, raster);
        } catch (const std::exception& ex) {
            failRead(data_, tFrameIndex, channel, zSliceIndex, ex.what());
        }
        if (raster.cols != data_.width || raster.rows != data_.height || raster.channels() != 1) {
            failRead(data_, tFrameIndex, channel, zSliceIndex,
                     "TIFF raster does not match the header checked at open");
        }
        cv::Mat region = raster(blockRect).clone();
        if (region.size() != blockSize) {
            cv::Mat scaled;
            Tools::resize(region, scaled, blockSize, cv::INTER_NEAREST);
            region = std::move(scaled);
        }
        parts.push_back(std::move(region));
    }

    if (parts.size() == 1) {
        parts[0].copyTo(output);
        return;
    }
    cv::merge(parts, output);
}
