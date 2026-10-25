#pragma once
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
namespace clashflux::ui {
// Exclusive temporary directory; the system picker copies this immutable
// redacted report. The application task owns it even if the dialog unmounts.
class DiagnosticExportFile {
public:
    static std::shared_ptr<DiagnosticExportFile> Prepare(const std::filesystem::path& root,const std::string& report) {
        if(report.size()>128*1024)throw std::runtime_error("Diagnostic report exceeds export limit");
        auto result=std::make_shared<DiagnosticExportFile>();
        for(unsigned attempt=0;attempt<8;++attempt) {
            const auto directory=root/(".health-export-"+std::to_string(std::random_device{}()));
            std::error_code ec;
            if(std::filesystem::create_directory(directory,ec)) {
                result->directory_=directory;result->path_=directory/"health.txt";
                std::ofstream out(result->path_,std::ios::binary|std::ios::trunc);out<<report;out.close();
                if(!out)throw std::runtime_error("Cannot prepare diagnostic export");return result;
            }
            if(ec&&ec!=std::errc::file_exists)break;
        }
        throw std::runtime_error("Cannot create diagnostic export directory");
    }
    ~DiagnosticExportFile(){std::error_code ec;if(!path_.empty())std::filesystem::remove(path_,ec);
        if(!directory_.empty())std::filesystem::remove(directory_,ec);}
    const std::filesystem::path& Path() const {return path_;}
private:
    std::filesystem::path directory_,path_;
};
}
