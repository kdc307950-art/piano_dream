#include "ui/FileDialog.h"

#include "portable-file-dialogs.h"

namespace piano {

struct FileDialog::Impl {
    explicit Impl(const std::string& title)
        : dlg(title, "", { "MIDI", "*.mid *.midi" }) {}
    pfd::open_file dlg;
};

FileDialog::FileDialog(const std::string& title)
    : impl_(std::make_unique<Impl>(title)) {}

FileDialog::~FileDialog() = default;

bool FileDialog::ready() const { return impl_->dlg.ready(0); }

std::vector<std::string> FileDialog::result() { return impl_->dlg.result(); }

void FileDialog::kill() { impl_->dlg.kill(); }

bool FileDialog::available() { return pfd::settings::available(); }

} // namespace piano
