#include <gtest/gtest.h>

#include <slideio/slideio/scene.hpp>
#include <slideio/slideio/slide.hpp>
#include <slideio/slideio/slideio.hpp>

#include <opencv2/imgcodecs.hpp>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>

namespace {

std::filesystem::path makeRoot(const std::string& name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void writePlane(const std::filesystem::path& path, int width, int height, std::uint16_t value) {
    cv::Mat image(height, width, CV_16UC1, cv::Scalar(value));
    if (!cv::imwrite(path.string(), image)) {
        throw std::runtime_error("could not write " + path.string());
    }
}

std::string imageXml(const std::string& file, const std::string& well, int row, int column, int field,
                     int t, int c, int z, double timestamp) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(6);
    out << "<Image filename=\"" << file << "\" timestamp_sec=\"" << timestamp << "\">"
        << "<Well label=\"" << well << "\"><Row number=\"" << row << "\"/><Column number=\""
        << column << "\"/></Well>"
        << "<Identifier field_index=\"" << field << "\" time_index=\"" << t << "\" wave_index=\""
        << c << "\" z_index=\"" << z << "\"/>"
        << "</Image>";
    return out.str();
}

void writeXdce(const std::filesystem::path& path, int image_count, const std::string& images,
               const std::string& extra = {}) {
    std::ofstream out(path);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        << "<ImageStack>"
        << "<Application name=\"IN Cell Analyzer 2200\"/>"
        << "<Creation date=\"2019-07-26\" time=\"13:20:48\" timeZoneOffset=\"36000000\"/>"
        << "<AutoLeadAcquisitionProtocol>"
        << "<ObjectiveCalibration magnification=\"10\" numerical_aperture=\"0.45\" "
        << "objective_name=\"Nikon 10X\" pixel_width=\"1.95\" unit=\"um\"/>"
        << "<Wavelengths>"
        << "<Wavelength index=\"0\" name=\"DAPI\"><EmissionFilter wavelength=\"435\"/></Wavelength>"
        << "<Wavelength index=\"1\" name=\"FITC\"><EmissionFilter wavelength=\"511\"/></Wavelength>"
        << "</Wavelengths>"
        << "<Times><TimePoint index=\"0\" ms=\"0\"/><TimePoint index=\"1\" ms=\"2000\"/></Times>"
        << "</AutoLeadAcquisitionProtocol>"
        << extra
        << "<Images number=\"" << image_count << "\">" << images << "</Images>"
        << "</ImageStack>";
}

std::shared_ptr<slideio::Slide> openOrThrow(const std::filesystem::path& path) {
    return slideio::openSlide(path.string(), "INCELL");
}

}  // namespace

TEST(InCellDriver, ReadsDenseGridAndIgnoresXmlOrder) {
    const auto root = makeRoot("slideio-incell-grid");
    std::string images;
    int count = 0;
    for (int t = 1; t >= 0; --t) {
        for (int c = 1; c >= 0; --c) {
            for (int z = 1; z >= 0; --z) {
                const std::string file = "p" + std::to_string(t) + std::to_string(c) +
                                         std::to_string(z) + ".tif";
                writePlane(root / file, 4, 6, static_cast<std::uint16_t>(1000 + t * 100 + c * 10 + z));
                images += imageXml(file, "A - 4", 1, 4, 0, t, c, z,
                                   100.25 + t * 2.0 + c * 0.1 + z * 0.01);
                ++count;
            }
        }
    }
    images = imageXml("b.tif", "B - 1", 1, 1, 0, 0, 0, 0, 200.0) + images;
    writePlane(root / "b.tif", 4, 6, 7);
    const auto xdce = root / "grid.xdce";
    writeXdce(xdce, count + 1, images);

    const auto slide = openOrThrow(xdce);
    ASSERT_EQ(slide->getNumScenes(), 2);
    EXPECT_EQ(slide->getScene(0)->getName(), "B - 1 f0");
    const auto scene = slide->getScene(1);
    EXPECT_EQ(scene->getName(), "A - 4 f0");
    EXPECT_EQ(std::get<2>(scene->getRect()), 4);
    EXPECT_EQ(std::get<3>(scene->getRect()), 6);
    EXPECT_EQ(scene->getNumChannels(), 2);
    EXPECT_EQ(scene->getNumTFrames(), 2);
    EXPECT_EQ(scene->getNumZSlices(), 2);
    EXPECT_EQ(scene->getChannelName(0), "DAPI");
    EXPECT_EQ(scene->getChannelName(1), "FITC");
    EXPECT_DOUBLE_EQ(scene->getChannelAttributes()[1]["EmissionWavelength"].asDouble(), 511.0);
    EXPECT_NEAR(std::get<0>(scene->getResolution()), 1.95e-6, 1e-9);
    EXPECT_DOUBLE_EQ(scene->getMagnification(), 10.0);
    EXPECT_DOUBLE_EQ(scene->getTFrameResolution(), 2.0);
    EXPECT_TRUE(scene->hasPlaneTimestamps());
    EXPECT_EQ(scene->getAcquisitionTime(), 100);
    EXPECT_NEAR(scene->getPlaneTimestamp(0, 1, 0), 0.35, 1e-6);
    EXPECT_NE(slide->getRawMetadata().find("\"ObjectiveName\":\"Nikon 10X\""), std::string::npos);
    EXPECT_NE(slide->getRawMetadata().find("2019-07-26T13:20:48+10:00"), std::string::npos);

    std::uint16_t pixel = 0;
    scene->read4DBlockChannels({0, 0, 1, 1}, {1}, {1, 2}, {1, 2}, &pixel, sizeof(pixel));
    EXPECT_EQ(pixel, 1000 + 100 + 10 + 1);

    const auto moved = makeRoot("slideio-incell-moved");
    std::filesystem::copy(root, moved, std::filesystem::copy_options::recursive);
    const auto moved_scene = openOrThrow(moved / "grid.xdce")->getScene(1);
    std::uint16_t moved_pixel = 0;
    moved_scene->read4DBlockChannels({0, 0, 1, 1}, {1}, {1, 2}, {1, 2}, &moved_pixel, sizeof(moved_pixel));
    EXPECT_EQ(moved_pixel, pixel);
    EXPECT_EQ(moved_scene->getName(), scene->getName());
    EXPECT_NEAR(std::get<0>(moved_scene->getResolution()), std::get<0>(scene->getResolution()), 1e-12);
}

TEST(InCellDriver, RejectsIncompleteOrUnsafeDatasets) {
    const auto root = makeRoot("slideio-incell-failures");
    writePlane(root / "a.tif", 4, 4, 1);
    writePlane(root / "b.tif", 4, 4, 2);

    auto expect = [&](const std::string& images, int count, const char* fragment, const std::string& extra = {}) {
        const auto path = root / "bad.xdce";
        writeXdce(path, count, images, extra);
        try {
            openOrThrow(path);
            FAIL() << "expected failure containing " << fragment;
        } catch (const std::exception& ex) {
            EXPECT_NE(std::string(ex.what()).find(fragment), std::string::npos) << ex.what();
        }
    };

    expect(imageXml("a.tif", "A", 1, 1, 0, 0, 0, 0, 10) +
               imageXml("b.tif", "A", 1, 1, 0, 2, 0, 0, 12),
           2, "missing plane");
    expect(imageXml("a.tif", "A", 1, 1, 0, 0, 0, 0, 10) +
               imageXml("b.tif", "A", 1, 1, 0, 0, 0, 0, 11),
           2, "duplicate plane");
    expect(imageXml("a.tif", "A", 1, 1, 0, 0, 0, 0, 10), 9, "Images number");
    expect(imageXml("a.tif", "A", 1, 1, 0, 1, 0, 0, 10) +
               imageXml("b.tif", "A", 1, 1, 0, 0, 0, 0, 12),
           2, "do not increase");
    expect(imageXml("/tmp/a.tif", "A", 1, 1, 0, 0, 0, 0, 10), 1, "absolute");
    expect(imageXml("../a.tif", "A", 1, 1, 0, 0, 0, 0, 10), 1, "escapes");
}

TEST(InCellDriver, RejectsSymlinkEscape) {
    const auto root = makeRoot("slideio-incell-link");
    const auto outside = makeRoot("slideio-incell-outside");
    writePlane(outside / "secret.tif", 4, 4, 1);
    std::filesystem::create_symlink(outside / "secret.tif", root / "linked.tif");
    const auto path = root / "link.xdce";
    writeXdce(path, 1, imageXml("linked.tif", "A", 1, 1, 0, 0, 0, 0, 10));
    try {
        openOrThrow(path);
        FAIL();
    } catch (const std::exception& ex) {
        EXPECT_NE(std::string(ex.what()).find("escapes"), std::string::npos) << ex.what();
    }
}

TEST(InCellDriver, ReadsRealFixture) {
    const char* fixture = std::getenv("STC_INCELL_FIXTURE");
    ASSERT_TRUE(fixture && *fixture) << "STC_INCELL_FIXTURE must point at the T8 x C4 fixture";
    const auto xdce = std::filesystem::path(fixture);
    std::filesystem::path file = xdce;
    if (std::filesystem::is_directory(xdce)) {
        file.clear();
        for (const auto& entry : std::filesystem::directory_iterator(xdce)) {
            if (entry.path().extension() == ".xdce") {
                file = entry.path();
            }
        }
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(file)) << file;
    const auto scene = openOrThrow(file)->getScene(0);
    EXPECT_EQ(std::get<2>(scene->getRect()), 682);
    EXPECT_EQ(std::get<3>(scene->getRect()), 682);
    EXPECT_EQ(scene->getNumChannels(), 4);
    EXPECT_EQ(scene->getNumTFrames(), 8);
    EXPECT_EQ(scene->getNumZSlices(), 1);
    EXPECT_EQ(scene->getChannelDataType(0), slideio::DataType::DT_UInt16);
    EXPECT_NEAR(std::get<0>(scene->getResolution()) * 1e6, 1.95, 1e-6);
    EXPECT_NEAR(scene->getTFrameResolution(), 2.0, 1e-6);
    std::uint16_t pixel = 0;
    EXPECT_NO_THROW(
        scene->read4DBlockChannels({0, 0, 1, 1}, {0}, {0, 1}, {0, 1}, &pixel, sizeof(pixel)));
}
