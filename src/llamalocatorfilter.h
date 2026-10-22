#pragma once

namespace Core {
class ILocatorFilter;
}

namespace LlamaCpp {
void setupLocatorFilter();

//! The llama.cpp locator filter instance (registered by setupLocatorFilter).
//! Used to open the locator directly, e.g. from the selection prompt button.
Core::ILocatorFilter *locatorFilter();
} // namespace LlamaCpp
