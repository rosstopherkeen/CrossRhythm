#pragma once

#include <cstdint>

#include "components/themes/minimal/MinimalTheme.h"

namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = 445;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverImageWidth = 296;
constexpr int homeCoverImageHeight = 444;
}  // namespace DashboardMetrics

// Garden home: the dashboard's book, stats and footer, laid out around a plant that grows with
// progress (see components/themes/garden/GardenGrowth.h).
namespace GardenMetrics {
constexpr int homeCoverImageWidth = 264;
constexpr int homeCoverImageHeight = 396;
}  // namespace GardenMetrics

class DashboardTheme : public MinimalTheme {
 public:
  // Dashboard: cover with the stats column beside it. Garden: a larger cover with a growing plant
  // and the progress line beside it, the stats in a table under the title.
  enum class Layout : uint8_t { Dashboard, Garden };

  explicit DashboardTheme(Layout layout = Layout::Dashboard) : layout_(layout) {}

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  void drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                       const GlobalReadingStats* globalStats, float progressPercent = -1.0f,
                       const char* currentChapterTitle = nullptr, bool inverted = false) const;

 private:
  Layout layout_;
};
