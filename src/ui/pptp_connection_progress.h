#pragma once

#include "ui.h"

namespace clashflux::ui {

// Both states must resolve from the installed catalog, including the less
// frequent disconnect path. Keep this shared with the resource mount regression.
inline huxerui::StringVariant PptpConnectionProgress(bool opening) {
    return opening ? Localized("连接中…") : Localized("断开中…");
}

} // namespace clashflux::ui
