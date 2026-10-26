#pragma once

#include "slideio/drivers/incell/incell_api_def.hpp"
#include "slideio/core/imagedriver.hpp"

namespace slideio {
namespace incell {

class SLIDEIO_INCELL_EXPORTS InCellImageDriver : public slideio::ImageDriver {
public:
    InCellImageDriver();
    ~InCellImageDriver() override;
    std::string getID() const override;
    std::shared_ptr<slideio::CVSlide> openFile(const std::string& filePath) override;
    std::string getFileSpecs() const override;
};

}  // namespace incell
}  // namespace slideio
