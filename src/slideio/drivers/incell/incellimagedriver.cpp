#include "slideio/drivers/incell/incellimagedriver.hpp"

#include "slideio/core/exceptions.hpp"
#include "slideio/drivers/incell/incellscene.hpp"
#include "slideio/imagetools/tifftools.hpp"

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <tuple>
#include <utility>

using namespace slideio;
using namespace slideio::incell;

namespace {

constexpr std::uint64_t kMaxXmlBytes = 64ull * 1024ull * 1024ull;
constexpr int kMaxImages = 200000;
constexpr int kMaxAxis = 100000;
constexpr std::size_t kMaxName = 4096;

[[noreturn]] void fail(const std::string& message) { RAISE_RUNTIME_ERROR << message; }

// Same shape as VSIFile::serializeMetadata: {tag, name, value, secondTag}.
nlohmann::json metaLeaf(const std::string& name, nlohmann::json value) {
    return nlohmann::json{{"tag", 0},
                          {"name", name},
                          {"value", std::move(value)},
                          {"secondTag", 0}};
}

nlohmann::json metaGroup(const std::string& name, nlohmann::json children) {
    return nlohmann::json{{"tag", 0},
                          {"name", name},
                          {"value", std::move(children)},
                          {"secondTag", 0}};
}

double parseDouble(const char* text, const char* what);

void pushText(nlohmann::json& fields, const char* label, const char* value) {
    if (value && *value) {
        fields.push_back(metaLeaf(label, std::string(value)));
    }
}

void pushNumber(nlohmann::json& fields, const char* label, const char* value) {
    if (value && *value) {
        fields.push_back(metaLeaf(label, parseDouble(value, label)));
    }
}

void appendPart(std::string& line, const std::string& part) {
    if (part.empty()) {
        return;
    }
    if (!line.empty()) {
        line += ", ";
    }
    line += part;
}

std::string formatWhole(double value) {
    if (std::abs(value - std::round(value)) < 1e-6) {
        return std::to_string(static_cast<long long>(std::llround(value)));
    }
    std::ostringstream out;
    out << value;
    return out.str();
}

bool parseFactor(const char*& text, double& value) {
    while (*text == ' ') {
        ++text;
    }
    char* end = nullptr;
    value = std::strtod(text, &end);
    if (end == text || !std::isfinite(value) || value <= 0.0) {
        return false;
    }
    text = end;
    return true;
}

// "3 X 3" → "3×3 merged". 1×1 is the unbinned sensor, so it adds nothing.
std::string formatBinning(const char* text) {
    if (!text || !*text) {
        return {};
    }
    const char* cursor = text;
    double across = 0.0;
    double down = 0.0;
    if (!parseFactor(cursor, across)) {
        return {};
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor == 'X' || *cursor == 'x') {
        ++cursor;
    } else if (static_cast<unsigned char>(cursor[0]) == 0xC3 &&
               static_cast<unsigned char>(cursor[1]) == 0x97) {
        cursor += 2;
    } else {
        return {};
    }
    if (!parseFactor(cursor, down)) {
        return {};
    }
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor != '\0') {
        return {};
    }
    if (std::abs(across - 1.0) < 1e-6 && std::abs(down - 1.0) < 1e-6) {
        return {};
    }
    return formatWhole(across) + "×" + formatWhole(down) + " merged";
}

std::string formatSensor(const char* width_text, const char* height_text) {
    if (!width_text || !*width_text || !height_text || !*height_text) {
        return {};
    }
    const char* width_cursor = width_text;
    const char* height_cursor = height_text;
    double width = 0.0;
    double height = 0.0;
    if (!parseFactor(width_cursor, width) || *width_cursor != '\0' ||
        !parseFactor(height_cursor, height) || *height_cursor != '\0') {
        return {};
    }
    return formatWhole(width) + "×" + formatWhole(height) + " sensor";
}

// Printed camera line: a real name, then how many sensor pixels become one output pixel,
// then the sensor size. "Unknown - Unknown" is a placeholder and is left out.
std::string cameraLine(const tinyxml2::XMLElement* camera) {
    std::string line;
    if (const char* name = camera->Attribute("name"); name && *name &&
                                                         std::strcmp(name, "Unknown - Unknown") != 0) {
        line = name;
    }
    if (const auto* binning = camera->FirstChildElement("Binning")) {
        appendPart(line, formatBinning(binning->Attribute("value")));
    }
    if (const auto* size = camera->FirstChildElement("Size")) {
        appendPart(line, formatSensor(size->Attribute("width"), size->Attribute("height")));
    }
    return line;
}

template <typename T>
std::shared_ptr<const T> intern(std::vector<std::shared_ptr<const T>>& pool, T value) {
    for (const auto& existing : pool) {
        if (*existing == value) {
            return existing;
        }
    }
    auto created = std::make_shared<const T>(std::move(value));
    pool.push_back(created);
    return created;
}

std::string asciiLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string readFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        fail("INCELL cannot read " + path.string() + ": " + ec.message());
    }
    if (size == 0 || size > kMaxXmlBytes) {
        fail("INCELL XML size is outside 1.." + std::to_string(kMaxXmlBytes) + " bytes: " +
             path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("INCELL cannot open " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        fail("INCELL short read of " + path.string());
    }
    return bytes;
}

std::string declaredEncoding(const std::string& bytes) {
    const auto end = bytes.find('>');
    if (bytes.rfind("<?xml", 0) != 0 || end == std::string::npos) {
        return "utf-8";
    }
    const std::string decl = asciiLower(bytes.substr(0, end));
    const auto key = decl.find("encoding");
    if (key == std::string::npos) {
        return "utf-8";
    }
    const auto quote = decl.find_first_of("\"'", key);
    if (quote == std::string::npos) {
        fail("INCELL XML encoding declaration is malformed");
    }
    const auto close = decl.find(decl[quote], quote + 1);
    if (close == std::string::npos) {
        fail("INCELL XML encoding declaration is malformed");
    }
    return decl.substr(quote + 1, close - quote - 1);
}

std::string toUtf8(const std::string& bytes) {
    const std::string encoding = declaredEncoding(bytes);
    std::string text;
    if (encoding == "utf-8" || encoding == "us-ascii") {
        text = bytes;
    } else if (encoding == "iso-8859-1" || encoding == "latin1" || encoding == "latin-1") {
        text.reserve(bytes.size());
        for (unsigned char c : bytes) {
            if (c < 0x80) {
                text.push_back(static_cast<char>(c));
            } else {
                text.push_back(static_cast<char>(0xC0 | (c >> 6)));
                text.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        const auto pos = text.find("encoding=");
        if (pos != std::string::npos) {
            const auto quote = text.find_first_of("\"'", pos);
            const auto close = quote == std::string::npos ? std::string::npos : text.find(text[quote], quote + 1);
            if (quote != std::string::npos && close != std::string::npos) {
                text.replace(quote + 1, close - quote - 1, "UTF-8");
            }
        }
    } else {
        fail("INCELL XML encoding is not supported: " + encoding);
    }
    return text;
}

const char* attr(const tinyxml2::XMLElement* element, const char* name) {
    const char* value = element->Attribute(name);
    if (!value) {
        fail(std::string("INCELL element ") + element->Name() + " is missing " + name);
    }
    return value;
}

int parseIndex(const char* text, const char* what) {
    if (!text || !*text) {
        fail(std::string("INCELL missing ") + what);
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > kMaxAxis) {
        fail(std::string("INCELL ") + what + " is outside 0.." + std::to_string(kMaxAxis) + ": " +
             text);
    }
    return static_cast<int>(value);
}

double parseDouble(const char* text, const char* what) {
    if (!text || !*text) {
        fail(std::string("INCELL missing ") + what);
    }
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value)) {
        fail(std::string("INCELL ") + what + " is not a finite number: " + text);
    }
    return value;
}

double metersFromCalibration(const tinyxml2::XMLElement* calibration) {
    const double width = parseDouble(attr(calibration, "pixel_width"), "pixel_width");
    const char* unit_text = calibration->Attribute("unit");
    const std::string unit = asciiLower(unit_text ? unit_text : "µm");
    double meters = 0.0;
    if (unit == "um" || unit == "µm" || unit == "μm") {
        meters = width * 1e-6;
    } else if (unit == "nm") {
        meters = width * 1e-9;
    } else if (unit == "mm") {
        meters = width * 1e-3;
    } else if (unit == "m") {
        meters = width;
    } else {
        fail("INCELL pixel size unit is not supported: " + unit);
    }
    if (!(meters > 0.0)) {
        fail("INCELL pixel size must be positive");
    }
    return meters;
}

std::filesystem::path resolveTiff(const std::filesystem::path& root, const std::string& reference) {
    if (reference.empty() || reference.size() > kMaxName) {
        fail("INCELL TIFF reference is empty or too long: " + reference);
    }
    if (reference.find('\\') != std::string::npos || reference.find(':') != std::string::npos) {
        fail("INCELL TIFF reference must be a relative path inside the experiment directory: " +
             reference);
    }
    const std::filesystem::path relative(reference);
    if (relative.is_absolute()) {
        fail("INCELL TIFF reference is absolute: " + reference);
    }
    for (const auto& part : relative) {
        if (part == "..") {
            fail("INCELL TIFF reference escapes the experiment directory: " + reference);
        }
    }
    std::error_code ec;
    const auto root_canon = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        fail("INCELL cannot resolve experiment directory " + root.string());
    }
    const auto full = std::filesystem::weakly_canonical(root / relative, ec);
    if (ec || !std::filesystem::is_regular_file(full)) {
        fail("INCELL TIFF is missing: reference '" + reference + "' path '" +
             (root / relative).string() + "'");
    }
    const auto inside = std::filesystem::relative(full, root_canon, ec);
    if (ec || inside.empty() || *inside.begin() == "..") {
        fail("INCELL TIFF escapes the experiment directory: reference '" + reference + "' path '" +
             full.string() + "'");
    }
    return full;
}

struct RawImage {
    std::string reference;
    std::filesystem::path path;
    int row = 0;
    int column = 0;
    int field = 0;
    int t = 0;
    int c = 0;
    int z = 0;
    double timestamp_sec = 0.0;
    double acquisition_time_ms = 0.0;
    std::string excitation_filter;
    std::string emission_filter;
    InCellPlaneStats stats;
    InCellPlaneGeometry geometry;
    bool has_exposure = false;
    InCellPlaneExposure exposure;
    std::string well;
};

struct SceneKey {
    int row = 0;
    int column = 0;
    int field = 0;
    std::string well;
    bool operator<(const SceneKey& other) const {
        return std::tie(row, column, field, well) <
               std::tie(other.row, other.column, other.field, other.well);
    }
};

std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string fingerprintOf(const std::filesystem::path& xdce,
                          const std::vector<std::pair<std::string, std::filesystem::path>>& files) {
    std::ostringstream record;
    auto append = [&](const std::string& name, const std::filesystem::path& path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        const auto mtime = std::filesystem::last_write_time(path, ec);
        const auto ticks = static_cast<long long>(ec ? 0 : mtime.time_since_epoch().count());
        record << name << '\t' << (ec ? 0 : size) << '\t' << ticks << '\n';
    };
    append(xdce.filename().string(), xdce);
    for (const auto& file : files) {
        append(file.first, file.second);
    }
    std::ostringstream hex;
    hex << std::hex << std::setfill('0') << std::setw(16) << fnv1a64(record.str());
    return hex.str();
}

int checkedProduct(int a, int b, const char* what) {
    if (a <= 0 || b <= 0 || a > std::numeric_limits<int>::max() / b) {
        fail(std::string("INCELL axis product overflows: ") + what);
    }
    return a * b;
}

class InCellSlide final : public CVSlide {
public:
    InCellSlide(std::string path, std::string metadata, std::vector<std::shared_ptr<InCellScene>> scenes)
        : path_(std::move(path)), scenes_(std::move(scenes)) {
        m_rawMetadata = std::move(metadata);
        m_metadataFormat = MetadataFormat::JSON;
    }

    int getNumScenes() const override { return static_cast<int>(scenes_.size()); }
    std::string getFilePath() const override { return path_; }
    std::shared_ptr<CVScene> getScene(int index) const override {
        if (index < 0 || index >= getNumScenes()) {
            fail("INCELL scene index out of range: " + std::to_string(index));
        }
        return scenes_[static_cast<std::size_t>(index)];
    }

private:
    std::string path_;
    std::vector<std::shared_ptr<InCellScene>> scenes_;
};

const tinyxml2::XMLElement* child(const tinyxml2::XMLElement* parent, const char* name) {
    const auto* element = parent->FirstChildElement(name);
    if (!element) {
        fail(std::string("INCELL element ") + parent->Name() + " is missing " + name);
    }
    return element;
}

std::shared_ptr<CVSlide> openInCell(const std::string& file_path) {
    const std::filesystem::path xdce(file_path);
    if (!std::filesystem::is_regular_file(xdce)) {
        fail("INCELL input is not a file: " + file_path);
    }
    const std::string xml = toUtf8(readFile(xdce));
    tinyxml2::XMLDocument document;
    if (document.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS) {
        fail(std::string("INCELL XML parse failed: ") +
             (document.ErrorStr() ? document.ErrorStr() : "unknown error"));
    }
    const auto* root = document.RootElement();
    if (!root) {
        fail("INCELL XML has no root element");
    }

    struct AcquisitionWave {
        int id = 0;
        std::string emission_filter;
        std::string excitation_filter;
        int optical_z_sectioning = 0;
    };

    const tinyxml2::XMLElement* calibration = nullptr;
    const tinyxml2::XMLElement* application = nullptr;
    const tinyxml2::XMLElement* creation = nullptr;
    const tinyxml2::XMLElement* microscopy = nullptr;
    const tinyxml2::XMLElement* protocol_polychroic = nullptr;
    const tinyxml2::XMLElement* camera = nullptr;
    const tinyxml2::XMLElement* software_autofocus = nullptr;
    const tinyxml2::XMLElement* laser_autofocus = nullptr;
    std::string dataset_uuid;
    std::map<int, InCellChannel> channels;
    std::map<int, AcquisitionWave> acquisition_waves;
    std::map<int, double> time_ms;
    std::vector<std::pair<std::string, double>> performance_events;
    std::vector<RawImage> images;
    int declared_images = -1;

    const auto visit = [&](auto&& self, const tinyxml2::XMLElement* element) -> void {
        for (const auto* child_element = element->FirstChildElement(); child_element;
             child_element = child_element->NextSiblingElement()) {
            const std::string name = child_element->Name();
            if (name == "UUID" && dataset_uuid.empty()) {
                if (const char* value = child_element->Attribute("value")) {
                    dataset_uuid = value;
                }
            } else if (name == "ObjectiveCalibration") {
                if (calibration &&
                    metersFromCalibration(calibration) != metersFromCalibration(child_element)) {
                    fail("INCELL contains conflicting ObjectiveCalibration values");
                }
                calibration = child_element;
            } else if (name == "Application" && !application) {
                application = child_element;
            } else if (name == "Creation" && !creation) {
                creation = child_element;
            } else if (name == "Microscopy" && !microscopy) {
                microscopy = child_element;
            } else if (name == "Polychroic" && !protocol_polychroic) {
                // Wavelength polychroics are read inside that branch and not visited.
                protocol_polychroic = child_element;
            } else if (name == "Camera" && !camera) {
                camera = child_element;
            } else if (name == "SoftwareAutoFocus" && !software_autofocus) {
                software_autofocus = child_element;
            } else if (name == "LaserAutoFocus" && !laser_autofocus) {
                laser_autofocus = child_element;
            } else if (name == "Wavelength") {
                const int index = parseIndex(attr(child_element, "index"), "wavelength index");
                InCellChannel channel;
                const char* label = child_element->Attribute("name");
                channel.name = label && *label ? label : "CH" + std::to_string(index);
                if (const char* imaging_mode = child_element->Attribute("imaging_mode")) {
                    channel.imaging_mode = imaging_mode;
                }
                if (const auto* emission = child_element->FirstChildElement("EmissionFilter")) {
                    if (const char* filter = emission->Attribute("name")) {
                        channel.emission_filter = filter;
                    }
                    if (const char* wavelength = emission->Attribute("wavelength")) {
                        channel.emission_nm = parseDouble(wavelength, "emission wavelength");
                    }
                }
                if (const auto* excitation = child_element->FirstChildElement("ExcitationFilter")) {
                    if (const char* filter = excitation->Attribute("name")) {
                        channel.excitation_filter = filter;
                    }
                    if (const char* wavelength = excitation->Attribute("wavelength")) {
                        channel.excitation_nm = parseDouble(wavelength, "excitation wavelength");
                    }
                }
                if (const auto* exposure = child_element->FirstChildElement("Exposure")) {
                    if (const char* time = exposure->Attribute("time")) {
                        channel.exposure_ms = parseDouble(time, "exposure time");
                    }
                    if (const char* unit = exposure->Attribute("unit")) {
                        channel.exposure_unit = unit;
                    }
                }
                if (const auto* polychroic = child_element->FirstChildElement("Polychroic")) {
                    if (const char* poly_name = polychroic->Attribute("name")) {
                        channel.polychroic = poly_name;
                    }
                }
                if (channels.count(index) && (channels[index].name != channel.name ||
                                              channels[index].emission_nm != channel.emission_nm ||
                                              channels[index].excitation_nm != channel.excitation_nm)) {
                    fail("INCELL wavelength index " + std::to_string(index) + " is duplicated");
                }
                channels[index] = std::move(channel);
            } else if (name == "Wave") {
                AcquisitionWave wave;
                wave.id = parseIndex(attr(child_element, "id"), "wave id");
                if (const char* emission = child_element->Attribute("emission_filter")) {
                    wave.emission_filter = emission;
                }
                if (const char* excitation = child_element->Attribute("excitation_filter")) {
                    wave.excitation_filter = excitation;
                }
                if (const char* z_section = child_element->Attribute("optical_z_sectioning")) {
                    wave.optical_z_sectioning =
                        parseIndex(z_section, "optical_z_sectioning");
                }
                if (acquisition_waves.count(wave.id) &&
                    (acquisition_waves[wave.id].emission_filter != wave.emission_filter ||
                     acquisition_waves[wave.id].excitation_filter != wave.excitation_filter)) {
                    fail("INCELL wave id " + std::to_string(wave.id) + " is duplicated");
                }
                acquisition_waves[wave.id] = std::move(wave);
            } else if (name == "TimePoint") {
                const int index = parseIndex(attr(child_element, "index"), "time point index");
                const double ms = parseDouble(attr(child_element, "ms"), "time point ms");
                if (time_ms.count(index) && time_ms[index] != ms) {
                    fail("INCELL time point " + std::to_string(index) + " is duplicated");
                }
                time_ms[index] = ms;
            } else if (name == "Event") {
                // PerformanceEvents absolute epoch ms. FINISHED must be after every other
                // event when present (catches cropped fixtures that leave the original end).
                const char* type = child_element->Attribute("type");
                const char* time = child_element->Attribute("time");
                if (type && *type && time) {
                    performance_events.emplace_back(type, parseDouble(time, "PerformanceEvent time"));
                }
            } else if (name == "Images") {
                const int number = parseIndex(attr(child_element, "number"), "Images number");
                declared_images = declared_images < 0 ? number : declared_images + number;
                self(self, child_element);
            } else if (name == "Image") {
                if (static_cast<int>(images.size()) >= kMaxImages) {
                    fail("INCELL image count exceeds " + std::to_string(kMaxImages));
                }
                RawImage image;
                image.reference = attr(child_element, "filename");
                // Bio-Formats uses acquisition_time_ms / 1000 as elapsed time. On IN Cell
                // 2200 that attribute is the exposure duration and does not advance. The
                // acquisition clock is timestamp_sec. Store acquisition_time_ms for later
                // use; never treat it as a timeline.
                image.timestamp_sec =
                    parseDouble(attr(child_element, "timestamp_sec"), "timestamp_sec");
                if (image.timestamp_sec < 0.0) {
                    fail("INCELL timestamp_sec is negative: " + image.reference);
                }
                if (const char* acquisition_ms = child_element->Attribute("acquisition_time_ms")) {
                    image.acquisition_time_ms =
                        parseDouble(acquisition_ms, "acquisition_time_ms");
                    if (image.acquisition_time_ms < 0.0) {
                        fail("INCELL acquisition_time_ms is negative: " + image.reference);
                    }
                }
                if (const auto* excitation = child_element->FirstChildElement("ExcitationFilter")) {
                    if (const char* filter = excitation->Attribute("name")) {
                        image.excitation_filter = filter;
                    }
                }
                if (const auto* emission = child_element->FirstChildElement("EmissionFilter")) {
                    if (const char* filter = emission->Attribute("name")) {
                        image.emission_filter = filter;
                    }
                }
                if (const auto* offset = child_element->FirstChildElement("OffsetFromWellCenter")) {
                    image.geometry.has_offset = true;
                    if (const char* x = offset->Attribute("x")) {
                        image.geometry.offset_x = parseDouble(x, "offset x");
                    }
                    if (const char* y = offset->Attribute("y")) {
                        image.geometry.offset_y = parseDouble(y, "offset y");
                    }
                    if (const char* unit = offset->Attribute("unit")) {
                        image.geometry.offset_unit = unit;
                    }
                }
                if (const auto* focus = child_element->FirstChildElement("FocusPosition")) {
                    image.geometry.has_focus = true;
                    if (const char* z = focus->Attribute("z")) {
                        image.geometry.focus_z = parseDouble(z, "focus z");
                    }
                    if (const char* unit = focus->Attribute("unit")) {
                        image.geometry.focus_unit = unit;
                    }
                }
                if (const auto* plate = child_element->FirstChildElement("PlatePosition_um")) {
                    image.geometry.has_plate = true;
                    if (const char* x = plate->Attribute("x")) {
                        image.geometry.plate_x = parseDouble(x, "plate x");
                    }
                    if (const char* y = plate->Attribute("y")) {
                        image.geometry.plate_y = parseDouble(y, "plate y");
                    }
                }
                if (const auto* exposure = child_element->FirstChildElement("Exposure")) {
                    image.has_exposure = true;
                    if (const char* time = exposure->Attribute("time")) {
                        image.exposure.time = parseDouble(time, "image exposure");
                    }
                    if (const char* unit = exposure->Attribute("unit")) {
                        image.exposure.unit = unit;
                    }
                }
                if (const auto* stats = child_element->FirstChildElement("MinMaxMean")) {
                    image.stats.present = true;
                    if (const char* min_value = stats->Attribute("min")) {
                        image.stats.min_value = parseDouble(min_value, "min");
                    }
                    if (const char* max_value = stats->Attribute("max")) {
                        image.stats.max_value = parseDouble(max_value, "max");
                    }
                    if (const char* mean = stats->Attribute("mean")) {
                        image.stats.mean = parseDouble(mean, "mean");
                    }
                    if (const char* sbr = stats->Attribute("sbr")) {
                        image.stats.sbr = parseDouble(sbr, "sbr");
                    }
                }
                const auto* well = child(child_element, "Well");
                image.well = attr(well, "label");
                image.row = parseIndex(attr(child(well, "Row"), "number"), "row");
                image.column = parseIndex(attr(child(well, "Column"), "number"), "column");
                const auto* identifier = child(child_element, "Identifier");
                image.field = parseIndex(attr(identifier, "field_index"), "field_index");
                image.t = parseIndex(attr(identifier, "time_index"), "time_index");
                image.c = parseIndex(attr(identifier, "wave_index"), "wave_index");
                image.z = parseIndex(attr(identifier, "z_index"), "z_index");
                images.push_back(std::move(image));
            } else {
                self(self, child_element);
            }
        }
    };
    visit(visit, root);

    if (!calibration) {
        fail("INCELL XML has no ObjectiveCalibration");
    }
    if (images.empty()) {
        fail("INCELL XML has no Image records");
    }
    if (declared_images != static_cast<int>(images.size())) {
        fail("INCELL Images number is " + std::to_string(declared_images) + " but " +
             std::to_string(images.size()) + " Image records were found");
    }
    {
        bool have_finished = false;
        double finished_ms = 0.0;
        double max_other_ms = -std::numeric_limits<double>::infinity();
        bool have_other = false;
        for (const auto& event : performance_events) {
            if (event.first == "PLATE_SCAN_FINISHED") {
                if (have_finished) {
                    fail("INCELL PerformanceEvents has duplicate PLATE_SCAN_FINISHED");
                }
                have_finished = true;
                finished_ms = event.second;
            } else {
                have_other = true;
                max_other_ms = std::max(max_other_ms, event.second);
            }
        }
        if (have_finished && have_other && finished_ms <= max_other_ms) {
            fail("INCELL PLATE_SCAN_FINISHED must be after all other PerformanceEvents");
        }
    }

    const std::filesystem::path root_dir = xdce.parent_path();
    for (auto& image : images) {
        image.path = resolveTiff(root_dir, image.reference);
    }

    std::map<SceneKey, std::vector<RawImage>> grouped;
    for (auto& image : images) {
        SceneKey key{image.row, image.column, image.field, image.well};
        grouped[key].push_back(std::move(image));
    }

    const double pixel_m = metersFromCalibration(calibration);
    const double magnification =
        calibration->Attribute("magnification")
            ? parseDouble(attr(calibration, "magnification"), "magnification")
            : 0.0;
    const double numerical_aperture =
        calibration->Attribute("numerical_aperture")
            ? parseDouble(attr(calibration, "numerical_aperture"), "numerical_aperture")
            : 0.0;
    const char* objective = calibration->Attribute("objective_name");
    // <Application name> is the instrument. <Camera name> is the camera.
    // Device is the slot VSI uses for the camera. Its model is the printed line:
    // a real name, "<n>×<n> merged", and "<w>×<h> sensor".
    std::string creation_time;
    if (creation && creation->Attribute("date") && creation->Attribute("time")) {
        creation_time = std::string(creation->Attribute("date")) + "T" + creation->Attribute("time");
        if (const char* offset = creation->Attribute("timeZoneOffset")) {
            const double ms = parseDouble(offset, "timeZoneOffset");
            const int minutes_total = static_cast<int>(std::llround(ms / 60000.0));
            const int absolute = std::abs(minutes_total);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%c%02d:%02d", minutes_total < 0 ? '-' : '+',
                          absolute / 60, absolute % 60);
            creation_time += buf;
        }
    }

    std::vector<std::shared_ptr<InCellScene>> scenes;
    std::vector<std::pair<std::string, std::filesystem::path>> fingerprint_files;
    std::vector<std::shared_ptr<const InCellPlaneGeometry>> geometries;
    std::vector<std::shared_ptr<const InCellPlaneExposure>> exposures;
    std::uint64_t companion_bytes = 0;
    int scene_index = 0;
    for (auto& entry : grouped) {
        auto& group = entry.second;
        int max_t = 0;
        int max_c = 0;
        int max_z = 0;
        for (const auto& image : group) {
            max_t = std::max(max_t, image.t);
            max_c = std::max(max_c, image.c);
            max_z = std::max(max_z, image.z);
        }
        const int num_t = max_t + 1;
        const int num_c = max_c + 1;
        const int num_z = max_z + 1;
        const int count = checkedProduct(checkedProduct(num_t, num_c, "T*C"), num_z, "T*C*Z");
        InCellSceneData data;
        data.file_path = file_path;
        data.scene_index = scene_index++;
        data.name = entry.first.well + " f" + std::to_string(entry.first.field);
        data.num_t = num_t;
        data.num_c = num_c;
        data.num_z = num_z;
        data.magnification = magnification;
        data.resolution = Resolution(pixel_m, pixel_m);
        data.channels.resize(static_cast<std::size_t>(num_c));
        for (int c = 0; c < num_c; ++c) {
            auto found = channels.find(c);
            data.channels[static_cast<std::size_t>(c)] =
                found == channels.end() ? InCellChannel{"CH" + std::to_string(c), 0.0} : found->second;
        }
        data.planes.resize(static_cast<std::size_t>(count));
        std::vector<char> seen(static_cast<std::size_t>(count), 0);
        double earliest = std::numeric_limits<double>::infinity();
        for (const auto& image : group) {
            const int index = (image.t * num_c + image.c) * num_z + image.z;
            if (seen[static_cast<std::size_t>(index)]) {
                fail("INCELL duplicate plane scene='" + data.name + "' t=" + std::to_string(image.t) +
                     " c=" + std::to_string(image.c) + " z=" + std::to_string(image.z) + " file='" +
                     image.reference + "'");
            }
            seen[static_cast<std::size_t>(index)] = 1;
            if (image.c >= 0 && image.c < num_c) {
                auto& channel = data.channels[static_cast<std::size_t>(image.c)];
                if (!image.excitation_filter.empty()) {
                    if (!channel.excitation_filter.empty() &&
                        channel.excitation_filter != image.excitation_filter) {
                        fail("INCELL Image excitation filter '" + image.excitation_filter +
                             "' conflicts with wavelength '" + channel.excitation_filter +
                             "' for " + image.reference);
                    }
                    if (channel.excitation_filter.empty()) {
                        channel.excitation_filter = image.excitation_filter;
                    }
                }
                if (!image.emission_filter.empty()) {
                    if (!channel.emission_filter.empty() &&
                        channel.emission_filter != image.emission_filter) {
                        fail("INCELL Image emission filter '" + image.emission_filter +
                             "' conflicts with wavelength '" + channel.emission_filter + "' for " +
                             image.reference);
                    }
                    if (channel.emission_filter.empty()) {
                        channel.emission_filter = image.emission_filter;
                    }
                }
            }
            InCellPlane plane;
            plane.reference = image.reference;
            plane.path = image.path;
            plane.timestamp_sec = image.timestamp_sec;
            plane.acquisition_time_ms = image.acquisition_time_ms;
            plane.stats = image.stats;
            if (image.geometry.has_offset || image.geometry.has_focus || image.geometry.has_plate) {
                plane.geometry = intern(geometries, image.geometry);
            }
            if (image.has_exposure) {
                plane.exposure = intern(exposures, image.exposure);
            }
            data.planes[static_cast<std::size_t>(index)] = std::move(plane);
            earliest = std::min(earliest, image.timestamp_sec);
        }
        for (int i = 0; i < count; ++i) {
            if (!seen[static_cast<std::size_t>(i)]) {
                const int z = i % num_z;
                const int tc = i / num_z;
                const int c = tc % num_c;
                const int t = tc / num_c;
                fail("INCELL missing plane scene='" + data.name + "' t=" + std::to_string(t) +
                     " c=" + std::to_string(c) + " z=" + std::to_string(z));
            }
        }
        for (int z = 0; z < num_z; ++z) {
            for (int c = 0; c < num_c; ++c) {
                for (int t = 1; t < num_t; ++t) {
                    const double previous = data.planes[( (t - 1) * num_c + c) * num_z + z].timestamp_sec;
                    const double current = data.planes[(t * num_c + c) * num_z + z].timestamp_sec;
                    if (!(current > previous)) {
                        fail("INCELL timestamps do not increase with time at scene='" + data.name +
                             "' c=" + std::to_string(c) + " z=" + std::to_string(z));
                    }
                }
            }
        }
        data.acquisition_time = static_cast<int64_t>(std::floor(earliest));
        bool interval_known = num_t >= 2;
        double interval_ms = 0.0;
        for (int t = 0; t < num_t && interval_known; ++t) {
            if (!time_ms.count(t)) {
                interval_known = false;
            }
        }
        if (interval_known) {
            interval_ms = time_ms[1] - time_ms[0];
            interval_known = interval_ms > 0.0;
            for (int t = 1; t < num_t && interval_known; ++t) {
                if (std::abs((time_ms[t] - time_ms[t - 1]) - interval_ms) > 0.5) {
                    interval_known = false;
                }
            }
        }
        data.frame_interval_s = interval_known ? interval_ms / 1000.0 : 0.0;

        bool header_known = false;
        for (const auto& plane : data.planes) {
            std::vector<TiffDirectory> directories;
            try {
                TiffTools::scanFile(plane.path.string(), directories);
            } catch (const std::exception& ex) {
                fail("INCELL cannot scan TIFF '" + plane.reference + "' path '" +
                     plane.path.string() + "': " + ex.what());
            }
            if (directories.size() != 1 || directories[0].channels != 1 || directories[0].width <= 0 ||
                directories[0].height <= 0 || directories[0].dataType == DataType::DT_Unknown) {
                fail("INCELL TIFF must be a single-page, single-sample image: " + plane.reference);
            }
            if (!header_known) {
                data.width = directories[0].width;
                data.height = directories[0].height;
                data.data_type = directories[0].dataType;
                data.compression = directories[0].slideioCompression;
                header_known = true;
            } else if (directories[0].width != data.width || directories[0].height != data.height ||
                       directories[0].dataType != data.data_type) {
                fail("INCELL TIFF header does not match the scene: " + plane.reference);
            }
            std::error_code ec;
            companion_bytes += std::filesystem::file_size(plane.path, ec);
            fingerprint_files.emplace_back(plane.reference, plane.path);
        }
        scenes.push_back(std::make_shared<InCellScene>(std::move(data)));
    }

    std::sort(fingerprint_files.begin(), fingerprint_files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::string fingerprint = fingerprintOf(xdce, fingerprint_files);
    {
        // Project size = manifest + every companion TIFF referenced by the scenes.
        std::error_code ec;
        companion_bytes += std::filesystem::file_size(xdce, ec);
    }
    nlohmann::json children = nlohmann::json::array();
    if (!dataset_uuid.empty()) {
        children.push_back(metaLeaf("dataset_uuid", dataset_uuid));
    }
    children.push_back(metaGroup(
        "Optical properties",
        [&] {
            nlohmann::json fields = nlohmann::json::array(
                {metaLeaf("Objective Name", objective ? objective : ""),
                 metaLeaf("Magnification", magnification),
                 metaLeaf("Numerical Aperture", numerical_aperture)});
            pushNumber(fields, "Focal Length", calibration->Attribute("focal_length"));
            pushText(fields, "Objective ID", calibration->Attribute("id"));
            pushNumber(fields, "Linear Type", calibration->Attribute("lineartype"));
            pushNumber(fields, "Pixel Width", calibration->Attribute("pixel_width"));
            pushNumber(fields, "Pixel Height", calibration->Attribute("pixel_height"));
            pushNumber(fields, "Refractive Index", calibration->Attribute("refractive_index"));
            pushNumber(fields, "SAC Collar", calibration->Attribute("sac_collar"));
            pushText(fields, "Unit", calibration->Attribute("unit"));
            pushNumber(fields, "X Shift", calibration->Attribute("x_shift"));
            pushNumber(fields, "Y Shift", calibration->Attribute("y_shift"));
            pushNumber(fields, "Z Shift", calibration->Attribute("z_shift"));
            return fields;
        }()));
    if (microscopy) {
        nlohmann::json fields = nlohmann::json::array();
        pushText(fields, "Type", microscopy->Attribute("type"));
        if (!fields.empty()) {
            children.push_back(metaGroup("Microscopy", std::move(fields)));
        }
    }
    if (protocol_polychroic) {
        nlohmann::json fields = nlohmann::json::array();
        pushText(fields, "ID", protocol_polychroic->Attribute("id"));
        pushText(fields, "Name", protocol_polychroic->Attribute("name"));
        if (!fields.empty()) {
            children.push_back(metaGroup("Polychroic", std::move(fields)));
        }
    }
    if (camera) {
        nlohmann::json fields = nlohmann::json::array();
        pushText(fields, "Name", camera->Attribute("name"));
        pushText(fields, "Version", camera->Attribute("version"));
        pushText(fields, "Apply FFC", camera->Attribute("apply_ffc"));
        pushText(fields, "Use FFC", camera->Attribute("use_ffc"));
        if (const auto* binning = camera->FirstChildElement("Binning")) {
            pushText(fields, "Binning", binning->Attribute("value"));
        }
        if (const auto* bias = camera->FirstChildElement("Bias")) {
            pushNumber(fields, "Bias", bias->Attribute("value"));
        }
        if (const auto* gain = camera->FirstChildElement("Gain")) {
            pushNumber(fields, "Gain", gain->Attribute("value"));
        }
        if (const auto* size = camera->FirstChildElement("Size")) {
            pushNumber(fields, "Width", size->Attribute("width"));
            pushNumber(fields, "Height", size->Attribute("height"));
        }
        if (const auto* roi = camera->FirstChildElement("ROI")) {
            pushNumber(fields, "ROI Columns", roi->Attribute("columns"));
            pushNumber(fields, "ROI Rows", roi->Attribute("rows"));
        }
        if (!fields.empty()) {
            children.push_back(metaGroup("Camera", std::move(fields)));
        }
    }
    if (software_autofocus) {
        nlohmann::json fields = nlohmann::json::array();
        pushText(fields, "Enabled", software_autofocus->Attribute("enabled"));
        pushText(fields, "Focus Channel", software_autofocus->Attribute("focus_channel"));
        pushText(fields, "Mode Type", software_autofocus->Attribute("mode_type"));
        pushText(fields, "Mode Value", software_autofocus->Attribute("mode_value"));
        pushNumber(fields, "Initial Stepping", software_autofocus->Attribute("init_stepping"));
        pushNumber(fields, "Minimum Stepping", software_autofocus->Attribute("min_stepping"));
        pushNumber(fields, "Maximum Search Range", software_autofocus->Attribute("max_search_range"));
        pushText(fields, "Refocus Each Time Point",
                 software_autofocus->Attribute("refocusEachTimePoint"));
        pushText(fields, "Show Live Image", software_autofocus->Attribute("show_live_image"));
        pushNumber(fields, "Skip Field", software_autofocus->Attribute("skip_field"));
        pushNumber(fields, "Skip Well", software_autofocus->Attribute("skip_well"));
        if (!fields.empty()) {
            children.push_back(metaGroup("Software autofocus", std::move(fields)));
        }
    }
    if (laser_autofocus) {
        nlohmann::json fields = nlohmann::json::array();
        pushText(fields, "Enabled", laser_autofocus->Attribute("enabled"));
        pushNumber(fields, "Power Level", laser_autofocus->Attribute("power_level"));
        if (!fields.empty()) {
            children.push_back(metaGroup("Laser autofocus", std::move(fields)));
        }
    }
    if (application) {
        nlohmann::json fields = nlohmann::json::array();
        pushText(fields, "Name", application->Attribute("name"));
        if (!fields.empty()) {
            children.push_back(metaGroup("Application", std::move(fields)));
        }
    }
    if (camera) {
        const std::string line = cameraLine(camera);
        if (!line.empty()) {
            children.push_back(metaGroup(
                "Device",
                nlohmann::json::array({metaLeaf("Device Model", line), metaLeaf("Device Name", line),
                                       metaLeaf("Device Subtype", 0)})));
        }
    }
    if (!creation_time.empty()) {
        children.push_back(metaLeaf("Creation time", creation_time));
    }
    if (!channels.empty()) {
        nlohmann::json wavelength_nodes = nlohmann::json::array();
        for (const auto& entry : channels) {
            const int index = entry.first;
            const InCellChannel& channel = entry.second;
            nlohmann::json fields = nlohmann::json::array(
                {metaLeaf("Index", index), metaLeaf("Name", channel.name)});
            if (!channel.excitation_filter.empty()) {
                fields.push_back(metaLeaf("Excitation Filter", channel.excitation_filter));
            }
            if (channel.excitation_nm > 0.0) {
                fields.push_back(metaLeaf("Excitation Wavelength", channel.excitation_nm));
            }
            if (!channel.emission_filter.empty()) {
                fields.push_back(metaLeaf("Emission Filter", channel.emission_filter));
            }
            if (channel.emission_nm > 0.0) {
                fields.push_back(metaLeaf("Emission Wavelength", channel.emission_nm));
            }
            if (channel.exposure_ms > 0.0) {
                fields.push_back(metaLeaf("Exposure", channel.exposure_ms));
            }
            if (!channel.exposure_unit.empty()) {
                fields.push_back(metaLeaf("Exposure Unit", channel.exposure_unit));
            }
            if (!channel.polychroic.empty()) {
                fields.push_back(metaLeaf("Polychroic", channel.polychroic));
            }
            if (!channel.imaging_mode.empty()) {
                fields.push_back(metaLeaf("Imaging Mode", channel.imaging_mode));
            }
            wavelength_nodes.push_back(metaGroup("Wavelength", std::move(fields)));
        }
        children.push_back(metaGroup("Wavelengths", std::move(wavelength_nodes)));
    }
    if (!acquisition_waves.empty()) {
        nlohmann::json wave_nodes = nlohmann::json::array();
        for (const auto& entry : acquisition_waves) {
            const AcquisitionWave& wave = entry.second;
            wave_nodes.push_back(metaGroup(
                "Wave",
                nlohmann::json::array(
                    {metaLeaf("ID", wave.id), metaLeaf("Excitation Filter", wave.excitation_filter),
                     metaLeaf("Emission Filter", wave.emission_filter),
                     metaLeaf("Optical Z Sectioning", wave.optical_z_sectioning)})));
        }
        children.push_back(metaGroup("Acquisition waves", std::move(wave_nodes)));
    }
    children.push_back(metaLeaf("companion_file_count",
                                static_cast<std::int64_t>(fingerprint_files.size())));
    children.push_back(metaLeaf("companion_bytes", static_cast<std::uint64_t>(companion_bytes)));
    children.push_back(metaLeaf("dataset_fingerprint", fingerprint));
    children.push_back(metaLeaf("format", "xdce"));

    const nlohmann::json metadata =
        metaGroup("IN Cell", std::move(children));
    return std::make_shared<InCellSlide>(file_path, metadata.dump(), std::move(scenes));
}

}  // namespace

InCellImageDriver::InCellImageDriver() = default;
InCellImageDriver::~InCellImageDriver() = default;

std::string InCellImageDriver::getID() const { return "INCELL"; }

std::shared_ptr<CVSlide> InCellImageDriver::openFile(const std::string& filePath) {
    return openInCell(filePath);
}

std::string InCellImageDriver::getFileSpecs() const { return "*.xdce"; }
