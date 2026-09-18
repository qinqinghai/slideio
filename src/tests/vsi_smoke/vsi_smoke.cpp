#include "slideio/slideio/slideio.hpp"
#include "slideio/slideio/scene.hpp"
#include "slideio/core/levelinfo.hpp"
#include "slideio/core/tools/tools.hpp"
#include "slideio/core/tools/cvtools.hpp"
#include "slideio/imagetools/imagetools.hpp"
#include "slideio/drivers/vsi/vsifile.hpp"
#include "slideio/drivers/vsi/etsfile.hpp"

#include <openjpeg.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

bool bufferHasNonZero(const std::vector<uint8_t>& buffer)
{
    for (uint8_t b : buffer) {
        if (b != 0) {
            return true;
        }
    }
    return false;
}

void testUtf16()
{
    expect(slideio::Tools::fromUnicode16(u"").empty(), "empty UTF-16 should yield empty UTF-8");
    expect(slideio::Tools::fromUnicode16(u"Hello, World!") == "Hello, World!", "BMP ASCII mismatch");
    const std::string helloWorldJp = "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\xE4\xB8\x96\xE7\x95\x8C";
    expect(slideio::Tools::fromUnicode16(u"\u3053\u3093\u306B\u3061\u306F\u4E16\u754C") == helloWorldJp,
        "BMP CJK mismatch");

    std::u16string grin;
    grin.push_back(char16_t(0xD83D));
    grin.push_back(char16_t(0xDE00));
    expect(slideio::Tools::fromUnicode16(grin) == "\xF0\x9F\x98\x80", "surrogate pair mismatch");
    std::cout << "utf16: ok" << std::endl;
}

void testJp2kRoundtrip()
{
    if (!opj_has_thread_support()) {
        fail("OpenJPEG was built without thread support");
    }
    std::cout << "openjpeg threads: yes, cpus=" << opj_get_num_cpus() << std::endl;

    cv::Mat src(32, 32, CV_8UC3);
    for (int y = 0; y < src.rows; ++y) {
        for (int x = 0; x < src.cols; ++x) {
            src.at<cv::Vec3b>(y, x) = cv::Vec3b(
                static_cast<uint8_t>(x * 4),
                static_cast<uint8_t>(y * 4),
                static_cast<uint8_t>(x + y));
        }
    }
    std::vector<uint8_t> encoded(64 * 1024);
    slideio::JP2KEncodeParameters params;
    const int n = slideio::ImageTools::encodeJp2KStream(src, encoded.data(), static_cast<int>(encoded.size()), params);
    expect(n > 0, "JPEG2000 encode produced no bytes");
    cv::Mat decoded;
    slideio::ImageTools::decodeJp2KStream(encoded.data(), static_cast<size_t>(n), decoded);
    expect(!decoded.empty(), "JPEG2000 decode produced an empty raster");
    expect(decoded.rows == src.rows && decoded.cols == src.cols, "JPEG2000 decode size mismatch");
    std::cout << "jp2k roundtrip: ok (" << n << " bytes)" << std::endl;
}

void dumpEtsFiles(const std::string& path)
{
    slideio::vsi::VSIFile vsiFile(path);
    std::cout << "vsi volumes: " << vsiFile.getNumVolumes()
              << " ets=" << vsiFile.getNumEtsFiles()
              << " expect_external=" << (vsiFile.expectExternalFiles() ? "yes" : "no")
              << std::endl;
    expect(vsiFile.getNumEtsFiles() > 0,
        "no ETS files bound; companion .ets may have been treated as orphan");
    for (int i = 0; i < vsiFile.getNumEtsFiles(); ++i) {
        const auto ets = vsiFile.getEtsFile(i);
        expect(static_cast<bool>(ets), "null ETS file");
        const auto size = ets->getSize();
        const auto complete = ets->getSizeWithCompleteTiles();
        const auto tile = ets->getTileSize();
        const auto fileBytes = std::filesystem::file_size(ets->getFilePath());
        std::cout << "  ets[" << i << "] " << ets->getFilePath()
                  << " size=" << size.width << "x" << size.height
                  << " tiles=" << complete.width << "x" << complete.height
                  << " tile=" << tile.width << "x" << tile.height
                  << " compression=" << slideio::CVTools::compressionToString(ets->getCompression())
                  << " channels=" << ets->getNumChannels()
                  << " z=" << ets->getNumZSlices()
                  << " t=" << ets->getNumTFrames()
                  << " levels=" << ets->getNumPyramidLevels()
                  << " bytes=" << fileBytes
                  << std::endl;
    }
}

void considerScene(const std::shared_ptr<slideio::Scene>& scene, const std::string& label,
    std::shared_ptr<slideio::Scene>& best, int& bestArea)
{
    expect(static_cast<bool>(scene), "scene is null: " + label);
    const auto rect = scene->getRect();
    const int width = std::get<2>(rect);
    const int height = std::get<3>(rect);
    const int area = width * height;
    std::cout << "  " << label << " name=" << scene->getName()
              << " " << width << "x" << height
              << " compression=" << slideio::CVTools::compressionToString(scene->getCompression())
              << " channels=" << scene->getNumChannels()
              << " z=" << scene->getNumZSlices()
              << " t=" << scene->getNumTFrames()
              << " levels=" << scene->getNumZoomLevels()
              << std::endl;
    for (int level = 0; level < scene->getNumZoomLevels(); ++level) {
        const slideio::LevelInfo* info = scene->getLevelInfo(level);
        if (info == nullptr) {
            continue;
        }
        const auto levelSize = info->getSize();
        const auto tileSize = info->getTileSize();
        std::cout << "    level[" << level << "] " << levelSize.width << "x" << levelSize.height
                  << " scale=" << info->getScale()
                  << " tile=" << tileSize.width << "x" << tileSize.height
                  << std::endl;
    }
    if (area > bestArea) {
        bestArea = area;
        best = scene;
    }
}

// Prefer a mid Z when the stack has multiple slices: edge planes are often empty.
int primaryZSlice(const std::shared_ptr<slideio::Scene>& scene)
{
    const int numZ = scene->getNumZSlices();
    return (numZ > 1) ? (numZ / 2) : 0;
}

void readSceneBlock(const std::shared_ptr<slideio::Scene>& scene, int zSlice, int tFrame)
{
    const auto rect = scene->getRect();
    const int width = std::get<2>(rect);
    const int height = std::get<3>(rect);
    expect(width > 0 && height > 0, "scene rectangle is empty");
    const int blockW = std::min(2048, width);
    const int blockH = std::min(2048, height);
    std::tuple<int, int, int, int> blockRect(0, 0, blockW, blockH);
    const int memSize = scene->getBlockSize(std::tuple<int, int>(blockW, blockH), 0, scene->getNumChannels(), 1, 1);
    std::vector<uint8_t> buffer(static_cast<size_t>(memSize));
    const auto t0 = std::chrono::steady_clock::now();
    if (zSlice == 0 && tFrame == 0 && scene->getNumZSlices() == 1 && scene->getNumTFrames() == 1) {
        scene->readBlock(blockRect, buffer.data(), buffer.size());
    } else {
        scene->read4DBlock(blockRect, std::tuple<int, int>(zSlice, zSlice + 1),
            std::tuple<int, int>(tFrame, tFrame + 1), buffer.data(), buffer.size());
    }
    const auto t1 = std::chrono::steady_clock::now();
    expect(bufferHasNonZero(buffer), "read block was all zeros");
    std::cout << "vsi read " << scene->getName() << " z=" << zSlice << " t=" << tFrame
              << " " << blockW << "x" << blockH << ": "
              << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms" << std::endl;
}

void testVsiSample(const std::string& path)
{
    dumpEtsFiles(path);

    const auto t0 = std::chrono::steady_clock::now();
    auto slide = slideio::openSlide(path, "VSI");
    const auto tOpen = std::chrono::steady_clock::now();
    expect(static_cast<bool>(slide), "openSlide returned null");
    expect(slide->getNumScenes() > 0, "slide has no scenes");
    std::cout << "vsi open: " << std::chrono::duration<double, std::milli>(tOpen - t0).count() << " ms" << std::endl;
    std::cout << "vsi scenes: " << slide->getNumScenes()
              << " aux=" << slide->getNumAuxImages() << std::endl;

    std::shared_ptr<slideio::Scene> best;
    int bestArea = 0;
    for (int i = 0; i < slide->getNumScenes(); ++i) {
        considerScene(slide->getScene(i), "scene[" + std::to_string(i) + "]", best, bestArea);
    }
    for (const auto& name : slide->getAuxImageNames()) {
        considerScene(slide->getAuxImage(name), "aux[" + name + "]", best, bestArea);
    }
    expect(static_cast<bool>(best), "no scene available to read");
    const int zRead = primaryZSlice(best);
    readSceneBlock(best, zRead, 0);
    if (best->getNumZSlices() > 1 && best->getNumZSlices() - 1 != zRead) {
        readSceneBlock(best, best->getNumZSlices() - 1, 0);
    }
    if (best->getNumTFrames() > 1) {
        readSceneBlock(best, zRead, best->getNumTFrames() - 1);
    }
}

} // namespace

int main()
{
    std::ios::sync_with_stdio(true);
    std::cout.setf(std::ios::unitbuf);
    try {
        testUtf16();
        testJp2kRoundtrip();
#ifdef SLIDEIO_VSI_SAMPLE
        testVsiSample(SLIDEIO_VSI_SAMPLE);
#else
        std::cout << "vsi sample: skipped (SLIDEIO_VSI_SAMPLE not set)" << std::endl;
#endif
        std::cout << "slideio_vsi_smoke: ok" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "slideio_vsi_smoke: " << ex.what() << std::endl;
        return 1;
    }
}
