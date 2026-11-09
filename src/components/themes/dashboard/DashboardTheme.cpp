#include "DashboardTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/themes/dashboard/HomeFooterLayout.h"
#include "components/themes/garden/GardenGrowth.h"
#include "fontIds.h"

namespace {
constexpr int kContentInsetX4 = 20;
constexpr int kContentInsetX3 = 75;
constexpr int kTopInset = 20;
constexpr int kCoverCornerRadius = 8;
constexpr int kStatsColumnWidth = 105;
constexpr int kStatsColumnWidthWide = 120;
constexpr int kCoverStatsGap = 15;
constexpr int kPairInwardShiftX3 = 15;
constexpr int kTitleTopGap = 28;
constexpr int kTitleChapterGap = 8;
constexpr int kBookTitleMaxLines = 2;
constexpr int kBookChapterMaxLines = 2;
constexpr int kFooterBottomGap = 57;
constexpr int kStatsRowCount = 7;
constexpr int kStatsRowCountX4 = 6;
constexpr int kStatsValueLabelGap = 1;

bool isWideScreen(const GfxRenderer& renderer) { return renderer.getScreenWidth() >= 560; }

int contentInset(const GfxRenderer& renderer) { return isWideScreen(renderer) ? kContentInsetX3 : kContentInsetX4; }

// Right edge of the stats column; the footer's reader icon ends on it too.
int contentRightEdge(const GfxRenderer& renderer) {
  return renderer.getScreenWidth() - contentInset(renderer) - (gpio.deviceIsX3() ? kPairInwardShiftX3 : 0);
}

Rect coverRectForScreen(const GfxRenderer& renderer, const Rect& rect) {
  const int inset = contentInset(renderer);
  const int statsW = isWideScreen(renderer) ? kStatsColumnWidthWide : kStatsColumnWidth;
  const int maxCoverW = renderer.getScreenWidth() - inset * 2 - statsW - kCoverStatsGap;
  const int coverW = std::min(DashboardMetrics::homeCoverImageWidth, maxCoverW);
  const int coverH = std::min(DashboardMetrics::homeCoverImageHeight, (coverW * 3) / 2);
  return Rect{inset + (gpio.deviceIsX3() ? kPairInwardShiftX3 : 0), rect.y + kTopInset, coverW, coverH};
}

Rect fittedBitmapRect(const Bitmap& bitmap, const Rect& target) {
  if (bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 || target.width <= 0 || target.height <= 0) {
    return target;
  }

  const float widthScale = static_cast<float>(target.width) / static_cast<float>(bitmap.getWidth());
  const float heightScale = static_cast<float>(target.height) / static_cast<float>(bitmap.getHeight());
  const float scale = std::min(1.0f, std::min(widthScale, heightScale));
  const int drawnW = std::min(target.width, std::max(1, static_cast<int>(std::ceil(bitmap.getWidth() * scale))));
  const int drawnH = std::min(target.height, std::max(1, static_cast<int>(std::ceil(bitmap.getHeight() * scale))));
  return Rect{target.x + (target.width - drawnW) / 2, target.y + (target.height - drawnH) / 2, drawnW, drawnH};
}

std::string coverPathForRect(const RecentBook& book, const Rect& imageRect) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    const std::string adaptivePath =
        Epub(book.path, "/.crosspoint").getAdaptiveThumbBmpPath(imageRect.width, imageRect.height);
    if (Storage.exists(adaptivePath.c_str())) {
      return adaptivePath;
    }
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.width, imageRect.height);
}

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                           Color::White);
  renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

  constexpr int iconSize = 32;
  drawLucideIcon(renderer, icon_book_open_32, coverRect.x + (coverRect.width - iconSize) / 2, coverRect.y + 36);

  constexpr int textPadding = 14;
  const int textW = coverRect.width - textPadding * 2;
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title, textW, 4, EpdFontFamily::BOLD);
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  int textY = coverRect.y + (coverRect.height - static_cast<int>(titleLines.size()) * lineH) / 2;
  for (const auto& line : titleLines) {
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, coverRect.x + (coverRect.width - lineW) / 2, textY, line.c_str(), true,
                      EpdFontFamily::BOLD);
    textY += lineH;
  }
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const Color backgroundColor) {
  bool hasCover = false;
  const std::string coverBmpPath = coverPathForRect(book, coverRect);
  if (!coverBmpPath.empty() && Storage.exists(coverBmpPath.c_str())) {
    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const Rect bitmapRect = fittedBitmapRect(bitmap, coverRect);
        renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                 backgroundColor);
        renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, kCoverCornerRadius,
                                 Color::White);
        renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
        renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height,
                                               kCoverCornerRadius, backgroundColor);
        renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1, kCoverCornerRadius,
                                 true);
        hasCover = true;
      }
      file.close();
    }
  }

  if (!hasCover) {
    drawMissingBookCover(renderer, coverRect, book);
  }
}

void drawRightAlignedText(const GfxRenderer& renderer, const int fontId, const int rightX, const int y,
                          const char* text, const bool bold = false, const bool black = true) {
  const EpdFontFamily::Style style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, rightX - width, y, text, black, style);
}

void formatCompactDuration(const uint32_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t minutes = (seconds + 30u) / 60u;
  if (minutes < 60) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
    return;
  }
  const uint32_t hours = minutes / 60u;
  const uint32_t remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%luh", static_cast<unsigned long>(hours));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(remainder));
  }
}

bool fallbackEstimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  seconds = 0;
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || stats.totalReadingSeconds < 120) {
    return false;
  }
  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(stats.totalReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }
  seconds = static_cast<uint32_t>(estimate + 0.5f);
  return seconds > 0;
}

bool estimatedTimeLeft(const BookReadingStats& stats, const float progressPercent, uint32_t& seconds) {
  if (stats.estimatedTimeLeftSeconds > 0) {
    seconds = stats.estimatedTimeLeftSeconds;
    return true;
  }
  return fallbackEstimatedTimeLeft(stats, progressPercent, seconds);
}

bool estimateFinishDateFromDailyPace(const BookReadingStats& stats, const ReadingStatsDateTime& today,
                                     const uint32_t estimatedReadingSeconds, ReadingStatsDate& outDate) {
  outDate = {};
  if (!today.isValid() || !stats.startDate.isValid() || estimatedReadingSeconds == 0 ||
      stats.totalReadingSeconds == 0) {
    return false;
  }

  const uint16_t elapsedDays = readingSpanDaysElapsed(stats.startDate, today.date);
  const uint16_t readingDays = std::max<uint16_t>(1, elapsedDays);
  const uint64_t estimatedCalendarSeconds =
      (static_cast<uint64_t>(estimatedReadingSeconds) * static_cast<uint64_t>(readingDays) * 86400ULL +
       static_cast<uint64_t>(stats.totalReadingSeconds) / 2ULL) /
      static_cast<uint64_t>(stats.totalReadingSeconds);
  if (estimatedCalendarSeconds == 0) {
    return false;
  }

  ReadingStatsDateTime estimatedFinish = today;
  addSecondsToReadingStatsDateTime(estimatedFinish,
                                   static_cast<uint32_t>(std::min<uint64_t>(estimatedCalendarSeconds, UINT32_MAX)));
  outDate = estimatedFinish.date;
  return outDate.isValid();
}

float pagesPerMinute(const uint32_t totalPagesTurned, const uint32_t totalReadingSeconds) {
  if (totalReadingSeconds <= 60) {
    return 0.0f;
  }
  return static_cast<float>(totalPagesTurned) * 60.0f / static_cast<float>(totalReadingSeconds);
}

const char* dayCountText(const uint16_t days) { return days == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS); }

int statsBlockHeight(const GfxRenderer& renderer) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  return valueLineH + kStatsValueLabelGap + labelLineH;
}

int statsBlockTop(const Rect& coverRect, const int index, const int blockH, const int rowCount) {
  const int remainingH = std::max(0, coverRect.height - blockH * rowCount);
  const int gapCount = rowCount - 1;
  const int gap = gapCount > 0 ? remainingH / gapCount : 0;
  const int remainder = gapCount > 0 ? remainingH % gapCount : 0;
  return coverRect.y + index * (blockH + gap) + std::min(index, remainder);
}

void drawStatsRow(const GfxRenderer& renderer, const int rightX, const int y, const char* value, const char* label,
                  const bool black = true) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  drawRightAlignedText(renderer, UI_12_FONT_ID, rightX, y, value, true, black);
  drawRightAlignedText(renderer, SMALL_FONT_ID, rightX, y + valueLineH + kStatsValueLabelGap, label, false, black);
}

// The book's stats as display strings, shared by the dashboard's column and the garden's table so
// both always show the same values. About 280 bytes: it lives on the render task's stack only while
// one home frame is drawn, and fixed buffers avoid heap churn on every redraw.
struct StatValues {
  char time[32];
  char timeLeft[32];
  char progress[16];
  char dailyAvg[32];
  char pagesPerMin[16];
  char sessions[16];
  char avgSession[32];
  char started[32];
  char startedLabel[48];
  char finish[24];
  const char* finishLabel = nullptr;
  bool rtc = false;
};

void computeStatValues(const BookReadingStats& bookStats, const float progressPercent, StatValues& v) {
  v.rtc = halClock.isAvailable();
  uint32_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeft(bookStats, progressPercent, estimatedSeconds);
  ReadingStatsDateTime today;
  const bool hasToday = v.rtc && getCurrentLocalReadingStatsDateTime(today);
  const ReadingStatsDate endDate = bookStats.isCompleted && bookStats.finishedDate.isValid()
                                       ? bookStats.finishedDate
                                       : (hasToday ? today.date : ReadingStatsDate{});
  const bool hasDaySpan = bookStats.startDate.isValid() && endDate.isValid();
  const uint16_t daysReading = hasDaySpan ? readingSpanDaysElapsed(bookStats.startDate, endDate) : 0;

  BookReadingStats::formatDuration(bookStats.totalReadingSeconds, v.time, sizeof(v.time));

  if (hasEstimate && !bookStats.isCompleted) {
    formatCompactDuration(estimatedSeconds, v.timeLeft, sizeof(v.timeLeft));
  } else {
    snprintf(v.timeLeft, sizeof(v.timeLeft), "-");
  }

  if (progressPercent >= 0.0f) {
    snprintf(v.progress, sizeof(v.progress), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(v.progress, sizeof(v.progress), "-");
  }

  if (hasDaySpan) {
    const uint16_t dailyAverageDays = std::max<uint16_t>(1, daysReading);
    BookReadingStats::formatDuration(bookStats.totalReadingSeconds / dailyAverageDays, v.dailyAvg, sizeof(v.dailyAvg));
  } else {
    snprintf(v.dailyAvg, sizeof(v.dailyAvg), "-");
  }

  snprintf(v.pagesPerMin, sizeof(v.pagesPerMin), "%.1f",
           pagesPerMinute(bookStats.totalPagesTurned, bookStats.totalReadingSeconds));

  snprintf(v.sessions, sizeof(v.sessions), "%u", static_cast<unsigned>(bookStats.sessionCount));
  const uint32_t avgSeconds = bookStats.sessionCount > 0 ? bookStats.totalReadingSeconds / bookStats.sessionCount : 0;
  BookReadingStats::formatDuration(avgSeconds, v.avgSession, sizeof(v.avgSession));

  if (hasDaySpan) {
    snprintf(v.started, sizeof(v.started), "%u %s", static_cast<unsigned>(daysReading), dayCountText(daysReading));
  } else {
    snprintf(v.started, sizeof(v.started), "-");
  }
  char startedDate[24];
  formatReadingStatsShortDate(bookStats.startDate, startedDate, sizeof(startedDate));
  snprintf(v.startedLabel, sizeof(v.startedLabel), "%s %s", tr(STR_STATS_STARTED), startedDate);

  ReadingStatsDate finishDisplayDate;
  if (bookStats.isCompleted) {
    finishDisplayDate = bookStats.finishedDate;
  } else if (hasToday && hasEstimate) {
    if (!estimateFinishDateFromDailyPace(bookStats, today, estimatedSeconds, finishDisplayDate)) {
      ReadingStatsDateTime estimatedFinish = today;
      addSecondsToReadingStatsDateTime(estimatedFinish, estimatedSeconds);
      finishDisplayDate = estimatedFinish.date;
    }
  }
  formatReadingStatsShortDate(finishDisplayDate, v.finish, sizeof(v.finish));
  v.finishLabel = bookStats.isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE);
}

void drawDashboardStats(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                        const float progressPercent, const bool showStats, const bool black = true) {
  const int rightX = contentRightEdge(renderer);
  const int blockH = statsBlockHeight(renderer);
  const bool showRtcStats = halClock.isAvailable();
  const int rowCount = showRtcStats ? kStatsRowCount : kStatsRowCountX4;
  const BookReadingStats emptyStats{};
  const BookReadingStats& bookStats = stats != nullptr ? *stats : emptyStats;
  if (!showStats) {
    char value[40];
    uint32_t estimatedSeconds = 0;
    const bool hasEstimate = estimatedTimeLeft(bookStats, progressPercent, estimatedSeconds);
    if (hasEstimate) {
      formatCompactDuration(estimatedSeconds, value, sizeof(value));
      drawStatsRow(renderer, rightX, coverRect.y + coverRect.height / 2 - blockH, value, tr(STR_TIME_LEFT_SHORT),
                   black);
    }
    if (progressPercent >= 0.0f) {
      snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
      const int progressY = hasEstimate ? coverRect.y + coverRect.height / 2 + blockH : coverRect.y;
      drawStatsRow(renderer, rightX, progressY, value, tr(STR_STATS_PROGRESS_LBL), black);
    }
    return;
  }

  StatValues v;
  computeStatValues(bookStats, progressPercent, v);
  int rowIndex = 0;
  const auto row = [&](const char* value, const char* label) {
    drawStatsRow(renderer, rightX, statsBlockTop(coverRect, rowIndex++, blockH, rowCount), value, label, black);
  };
  row(v.time, tr(STR_STATS_TIME_LBL));
  row(v.timeLeft, tr(STR_TIME_LEFT_SHORT));
  row(v.progress, tr(STR_STATS_PROGRESS_LBL));
  if (showRtcStats) row(v.dailyAvg, tr(STR_STATS_DAILY_AVG_LBL));
  row(v.pagesPerMin, tr(STR_STATS_PAGES_PER_MIN));
  if (!showRtcStats) {
    row(v.sessions, tr(STR_STATS_SESSIONS_LBL));
    row(v.avgSession, tr(STR_STATS_AVG_SESSION_LBL));
    return;
  }
  row(v.started, v.startedLabel);
  row(v.finish, v.finishLabel);
}

bool showBookStatsForPath(const std::string& path) {
  if (!SETTINGS.shouldTrackReadingStats()) return false;
  if (FsHelpers::hasEpubExtension(path))
    return BookStatsTracking::isBookEnabled(Epub::cachePathForFilePath(path, "/.crosspoint"));
  if (FsHelpers::hasXtcExtension(path))
    return BookStatsTracking::isBookEnabled(Xtc(path, "/.crosspoint").getCachePath());
  return false;
}

bool dominantReaderTypeBucket(const GlobalReadingStats& globalStats, ReadingTimeBucket& bucketOut) {
  const auto& values = globalStats.timeOfDaySeconds;
  const uint32_t totalSeconds = std::accumulate(values.begin(), values.end(), 0u);
  if (totalSeconds == 0) {
    return false;
  }

  const size_t dominantIndex =
      static_cast<size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())));
  bucketOut = static_cast<ReadingTimeBucket>(dominantIndex);
  return true;
}

const char* readerTypeLabel(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return tr(STR_STATS_NEW_READER);
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
    return tr(STR_STATS_NEW_READER);
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return tr(STR_STATS_MORNING_READER);
    case ReadingTimeBucket::Afternoon:
      return tr(STR_STATS_AFTERNOON_READER);
    case ReadingTimeBucket::Evening:
      return tr(STR_STATS_EVENING_READER);
    case ReadingTimeBucket::Night:
    default:
      return tr(STR_STATS_NIGHT_READER);
  }
}

const HomeFooterIcon& readerTypeIcon(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) {
    return HomeFooterNewReaderIcon;
  }

  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(*globalStats, bucket)) {
    return HomeFooterNewReaderIcon;
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return HomeFooterMorningIcon;
    case ReadingTimeBucket::Afternoon:
      return HomeFooterAfternoonIcon;
    case ReadingTimeBucket::Evening:
      return HomeFooterEveningIcon;
    case ReadingTimeBucket::Night:
    default:
      return HomeFooterNightIcon;
  }
}

uint16_t currentStreakDays(const GlobalReadingStats* globalStats) {
  if (globalStats == nullptr) return 0;
  ReadingStatsDateTime today;
  return getCurrentLocalReadingStatsDateTime(today) ? globalStats->currentReadingStreak(&today.date) : 0;
}

// Footer labels use Bitter 10 when Bitter is loaded. Scalable-font builds
// (S3) load the built-in Bitter only when it is the reader font, so otherwise
// the always-loaded UI font is used.
int footerLabelFontId(const GfxRenderer& renderer) {
  const auto& fonts = renderer.getFontMap();
  return fonts.find(BITTER_10_FONT_ID) != fonts.end() ? BITTER_10_FONT_ID : UI_10_FONT_ID;
}

void drawHomeFooterBitmap(const GfxRenderer& renderer, const uint8_t* bits, const int x, const int y, const int w,
                          const int h, const bool inverted) {
  if (inverted) {
    renderer.drawIconInverted(bits, x, y, w, h);
  } else {
    renderer.drawIcon(bits, x, y, w, h);
  }
}

// Two-line label whose second line sits on baselineY. alignRight puts each
// line's right edge at x; otherwise each line starts at x.
void drawFooterLabel(const GfxRenderer& renderer, const char* label, const int x, const int baselineY,
                     const int maxTextW, const bool alignRight, const bool inverted) {
  char firstLine[48];
  const char* line1 = nullptr;
  const char* line2 = nullptr;
  HomeFooter::splitLabel(label, firstLine, sizeof(firstLine), line1, line2);
  const int fontId = footerLabelFontId(renderer);
  const int secondTop = baselineY - renderer.getFontAscenderSize(fontId);
  const char* lines[2] = {line1, line2};
  const int tops[2] = {secondTop - HomeFooter::kLabelLineGap, secondTop};
  // Regular weight: the built-in families have only regular and bold, and bold
  // read too heavy next to the number.
  constexpr auto kStyle = EpdFontFamily::REGULAR;
  for (int i = 0; i < 2; ++i) {
    if (lines[i] == nullptr || lines[i][0] == '\0') continue;
    const std::string visible = renderer.truncatedText(fontId, lines[i], maxTextW, kStyle);
    const int w = renderer.getTextWidth(fontId, visible.c_str(), kStyle);
    renderer.drawText(fontId, alignRight ? x - w : x, tops[i], visible.c_str(), !inverted, kStyle);
  }
}

void drawLeftAnchoredFooterStat(const GfxRenderer& renderer, const int labelX, const int centerY, const int maxTextW,
                                const char* value, const char* label, const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int topY = centerY - totalH / 2;
  renderer.drawText(UI_12_FONT_ID, labelX + (labelW - valueW) / 2, topY, value, !inverted, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX, topY + valueLineH + kStatsValueLabelGap, visibleLabel.c_str(), !inverted);
}

void drawRightAnchoredFooterStat(const GfxRenderer& renderer, const int labelRightX, const int centerY,
                                 const int maxTextW, const char* value, const char* label,
                                 const bool inverted = false) {
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int totalH = valueLineH + kStatsValueLabelGap + labelLineH;
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(UI_10_FONT_ID, label, maxTextW);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, visibleLabel.c_str());
  const int labelX = labelRightX - labelW;
  const int topY = centerY - totalH / 2;
  renderer.drawText(UI_12_FONT_ID, labelX + (labelW - valueW) / 2, topY, value, !inverted, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, labelX, topY + valueLineH + kStatsValueLabelGap, visibleLabel.c_str(), !inverted);
}

// Vertical centre of the footer row; the garden's stats table hangs off it.
int footerCenterY(const GfxRenderer& renderer, const Rect& coverRect) {
  const int buttonHintReserve = gpio.hasTouch() ? 0 : DashboardMetrics::values.buttonHintsHeight;
  const int footerY = renderer.getScreenHeight() - buttonHintReserve - kFooterBottomGap;
  return std::max(coverRect.y + coverRect.height + 120, footerY);
}

void drawFooterStats(const GfxRenderer& renderer, const Rect& coverRect, const GlobalReadingStats* globalStats,
                     const bool inverted = false) {
  const int inset = contentInset(renderer);
  const int centerY = footerCenterY(renderer, coverRect);

  if (!halClock.isAvailable()) {
    char totalTime[40];
    char booksRead[16];
    const uint32_t totalReadingSeconds = globalStats != nullptr ? globalStats->totalReadingSeconds : 0;
    const uint32_t completedBooks = globalStats != nullptr ? globalStats->completedBooks : 0;
    BookReadingStats::formatDuration(totalReadingSeconds, totalTime, sizeof(totalTime));
    snprintf(booksRead, sizeof(booksRead), "%lu", static_cast<unsigned long>(completedBooks));

    const int halfW = renderer.getScreenWidth() / 2;
    const int maxTextW = std::max(1, halfW - inset * 2);
    drawLeftAnchoredFooterStat(renderer, coverRect.x, centerY, maxTextW, totalTime,
                               tr(STR_STATS_TOTAL_READING_TIME_LBL_SHORT), inverted);
    const int rightX = renderer.getScreenWidth() - inset;
    drawRightAnchoredFooterStat(renderer, rightX, centerY, maxTextW, booksRead, tr(STR_STATS_COMPLETED_LBL), inverted);
    return;
  }

  // Big streak number + "day / streak" on the left, "<time> / reader" + icon on
  // the right, all on one baseline (see HomeFooterLayout.h).
  const int baselineY = centerY + HomeFooter::kBaselineBelowCenter;
  const int halfW = renderer.getScreenWidth() / 2;

  // Number starts on the cover/title left edge.
  const HomeFooter::NumberLayout number = HomeFooter::layoutNumber(currentStreakDays(globalStats), coverRect.x, baselineY);
  for (int i = 0; i < number.count; ++i) {
    const HomeFooter::DigitPlacement& d = number.digits[i];
    drawHomeFooterBitmap(renderer, d.glyph->bits, d.x, d.y, d.glyph->width, d.glyph->height, inverted);
  }
  const int streakLabelX = number.inkRight + HomeFooter::kNumberLabelGap;
  drawFooterLabel(renderer, tr(STR_STATS_DAY_STREAK_LBL), streakLabelX, baselineY, std::max(1, halfW - streakLabelX),
                  false, inverted);

  const HomeFooterIcon& icon = readerTypeIcon(globalStats);
  const int iconX = HomeFooter::iconLeft(icon, contentRightEdge(renderer));
  drawHomeFooterBitmap(renderer, icon.bits, iconX, HomeFooter::iconTop(icon, baselineY), icon.width, icon.height,
                       inverted);
  const int readerLabelRight = iconX - HomeFooter::kIconLabelGap;
  drawFooterLabel(renderer, readerTypeLabel(globalStats), readerLabelRight, baselineY,
                  std::max(1, readerLabelRight - halfW), true, inverted);
}

void drawBookText(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                  const char* currentChapterTitle, const bool black = true) {
  const int inset = contentInset(renderer);
  const int textW = renderer.getScreenWidth() - inset * 2;
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, title, textW, kBookTitleMaxLines, EpdFontFamily::BOLD);
  int textY = coverRect.y + coverRect.height + kTitleTopGap;
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, coverRect.x, textY, line.c_str(), black, EpdFontFamily::BOLD);
    textY += titleLineH;
  }

  const char* subtitle =
      (currentChapterTitle != nullptr && currentChapterTitle[0] != '\0') ? currentChapterTitle : book.author.c_str();
  if (subtitle != nullptr && subtitle[0] != '\0') {
    auto subtitleLines = renderer.wrappedText(UI_12_FONT_ID, subtitle, textW, kBookChapterMaxLines);
    int subtitleY = textY + kTitleChapterGap;
    for (const auto& line : subtitleLines) {
      renderer.drawText(UI_12_FONT_ID, coverRect.x, subtitleY, line.c_str(), black);
      subtitleY += titleLineH;
    }
  }
}

// ---- Garden layout -------------------------------------------------------------------------------
// The W design (graphic-tooling tools/home-studies, round 14): a larger cover with the title and
// chapter under it, a plant column beside the cover with the progress line at its top, and the
// stats in a 3 x 2 ruled table above the footer.

constexpr int kGardenTopInset = 8;          // cover top below the status bar band
constexpr int kGardenColumnGap = 16;        // cover to plant column
constexpr int kGardenMinColumnWidth = 100;  // narrower than this and the cover gives way
constexpr int kGardenTitleTopGap = 18;
constexpr int kGardenPlantTopOffset = 12;   // lattice starts this far below the cover top
constexpr int kGardenTableFooterGap = 65;   // footer centre to the table's bottom rule
constexpr int kGardenTableTextGap = 6;      // last text line to the table's top rule

Rect gardenCoverRect(const GfxRenderer& renderer, const Rect& rect) {
  const int inset = contentInset(renderer);
  const int left = inset + (gpio.deviceIsX3() ? kPairInwardShiftX3 : 0);
  const int maxCoverW = contentRightEdge(renderer) - left - kGardenColumnGap - kGardenMinColumnWidth;
  const int coverW = std::min(GardenMetrics::homeCoverImageWidth, maxCoverW);
  const int coverH = std::min(GardenMetrics::homeCoverImageHeight, (coverW * 3) / 2);
  return Rect{left, rect.y + kGardenTopInset, coverW, coverH};
}

int gardenTableHeight(const GfxRenderer& renderer) {
  const int cellH = statsBlockHeight(renderer) + 4;
  return 4 + 2 * (cellH + 2);
}

int gardenTableTop(const GfxRenderer& renderer, const Rect& coverRect) {
  const int bottomRule = footerCenterY(renderer, coverRect) - kGardenTableFooterGap;
  return bottomRule - (gardenTableHeight(renderer) - 2);
}

struct TextInk {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // exclusive box of the progress line's black pixels
  int width = 0, height = 0;
  std::unique_ptr<uint8_t[]> bits;  // 1 = ink, row-major bit per pixel
  bool ink(const int x, const int y) const {
    if (x < x0 || y < y0 || x >= x1 || y >= y1) return false;
    const int i = (y - y0) * width + (x - x0);
    return (bits[i / 8] >> (i % 8)) & 1;
  }
};

struct GardenPixels {
  const GfxRenderer* renderer;
  static void set(void* ctx, const int x, const int y, const bool black) {
    static_cast<GardenPixels*>(ctx)->renderer->drawPixel(x, y, black);
  }
};

struct FrontLeafBand {
  int x0, x1, textBottom;
};

// The leaves that come over the progress line: rooted just below the text, only small ones and
// about a third of them, so their tips reach slightly over it (as in the W mockup).
bool frontLeaf(void* ctx, const float x, const float y, const float size) {
  const auto& b = *static_cast<FrontLeafBand*>(ctx);
  return x >= b.x0 - 4 && x <= b.x1 + 4 && y >= b.textBottom + 4 && y <= b.textBottom + 11 && size < 15 &&
         (static_cast<int>(x * 7 + y * 13) % 10) < 4;
}

// Plant column: the progress line at the top (its ink top on the cover's top edge, label left,
// value right, one baseline), the plant behind it with a 1 px paper halo, then the few leaves that
// come in front of it.
void drawGardenColumn(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                      const float progressPercent) {
  const int colX = coverRect.x + coverRect.width + kGardenColumnGap;
  const int colRight = contentRightEdge(renderer);
  if (colRight - colX < kGardenMinColumnWidth / 2) return;

  char value[16];
  if (progressPercent >= 0.0f) {
    snprintf(value, sizeof(value), "%d%%", static_cast<int>(progressPercent + 0.5f));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  const auto bounds = renderer.getTextVerticalBounds(UI_12_FONT_ID, value);
  const int valueY = coverRect.y - bounds.top;
  const int labelY = valueY + renderer.getFontAscenderSize(UI_12_FONT_ID) - renderer.getFontAscenderSize(SMALL_FONT_ID);
  const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const std::string label = renderer.truncatedText(SMALL_FONT_ID, tr(STR_STATS_PROGRESS_LBL),
                                                   std::max(1, colRight - colX - valueW - 8));
  renderer.drawText(SMALL_FONT_ID, colX, labelY, label.c_str(), true);
  renderer.drawText(UI_12_FONT_ID, colRight - valueW, valueY, value, true, EpdFontFamily::BOLD);

  // The text's own pixels, read back so the halo and the front leaves follow its real ink.
  TextInk text;
  text.x0 = colX;
  text.x1 = colRight;
  text.y0 = coverRect.y - 4;
  text.y1 = std::max(valueY + renderer.getLineHeight(UI_12_FONT_ID), labelY + renderer.getLineHeight(SMALL_FONT_ID));
  text.width = text.x1 - text.x0;
  text.height = text.y1 - text.y0;
  text.bits = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(text.width * text.height + 7) / 8);
  if (!text.bits) {
    LOG_ERR("HOME", "Garden: OOM for the progress line mask, plant skipped");
    return;
  }
  int inkBottom = text.y0;
  for (int y = text.y0; y < text.y1; ++y) {
    for (int x = text.x0; x < text.x1; ++x) {
      if (!renderer.isPixelBlack(x, y)) continue;
      const int i = (y - text.y0) * text.width + (x - text.x0);
      text.bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
      inkBottom = std::max(inkBottom, y + 1);
    }
  }

  garden::Plan plan;
  const garden::Column column{colX, coverRect.y + kGardenPlantTopOffset, colRight - colX,
                              coverRect.height - kGardenPlantTopOffset};
  if (!plan.build(column, garden::seedForPath(book.path.c_str()))) {
    LOG_ERR("HOME", "Garden: could not lay out the plant (OOM or column %dx%d)", column.width, column.height);
    return;
  }
  const float growth = progressPercent >= 0.0f ? progressPercent / 100.0f : 0.0f;
  GardenPixels pixels{&renderer};
  garden::Canvas canvas;
  canvas.ctx = &pixels;
  canvas.setPixel = &GardenPixels::set;
  canvas.clipX0 = colX - 12;  // leaves end raggedly in the gutter, never on the cover
  canvas.clipX1 = renderer.getScreenWidth() - 6;
  canvas.clipY0 = coverRect.y;  // tips above the cover's top edge are trimmed
  canvas.clipY1 = coverRect.y + coverRect.height + 2;
  if (!garden::draw(plan, growth, canvas)) {
    LOG_ERR("HOME", "Garden: OOM drawing the plant");
  }

  // 1 px paper halo around the text, then the text again from its own pixels.
  for (int y = text.y0; y < text.y1; ++y) {
    for (int x = text.x0; x < text.x1; ++x) {
      if (!text.ink(x, y)) continue;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) renderer.drawPixel(x + dx, y + dy, false);
      }
    }
  }
  for (int y = text.y0; y < text.y1; ++y) {
    for (int x = text.x0; x < text.x1; ++x) {
      if (text.ink(x, y)) renderer.drawPixel(x, y, true);
    }
  }

  // The leaves rooted just under the text, drawn again in front of it.
  FrontLeafBand band{text.x0, text.x1, inkBottom};
  garden::DrawOptions front;
  front.leavesOnly = true;
  front.leafFilter = &frontLeaf;
  front.filterCtx = &band;
  canvas.clipY0 = coverRect.y - 6;
  if (!garden::draw(plan, growth, canvas, front)) {
    LOG_ERR("HOME", "Garden: OOM drawing the front leaves");
  }
}

// Title and chapter under the cover, fitted to the space above the stats table.
void drawGardenBookText(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                        const char* currentChapterTitle, const int bottomY) {
  const int inset = contentInset(renderer);
  const int textW = renderer.getScreenWidth() - inset * 2;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  int textY = coverRect.y + coverRect.height + kGardenTitleTopGap;
  const int titleLines = std::min(kBookTitleMaxLines, std::max(0, (bottomY - textY) / lineH));
  if (titleLines == 0) return;
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, title, textW, titleLines, EpdFontFamily::BOLD)) {
    renderer.drawText(UI_12_FONT_ID, coverRect.x, textY, line.c_str(), true, EpdFontFamily::BOLD);
    textY += lineH;
  }
  const char* subtitle =
      (currentChapterTitle != nullptr && currentChapterTitle[0] != '\0') ? currentChapterTitle : book.author.c_str();
  textY += kTitleChapterGap;
  const int subtitleLines = std::min(kBookChapterMaxLines, std::max(0, (bottomY - textY) / lineH));
  if (subtitle == nullptr || subtitle[0] == '\0' || subtitleLines == 0) return;
  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, subtitle, textW, subtitleLines)) {
    renderer.drawText(UI_12_FONT_ID, coverRect.x, textY, line.c_str(), true);
    textY += lineH;
  }
}

// Stats as a 3 x 2 ruled table, columns as up/down pairs: time read / time left, pages per minute /
// daily average, started / estimated finish (sessions / average session without a clock).
void drawGardenStatsTable(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                          const float progressPercent) {
  const BookReadingStats emptyStats{};
  StatValues v;
  computeStatValues(stats != nullptr ? *stats : emptyStats, progressPercent, v);
  const char* values[2][3] = {{v.time, v.pagesPerMin, v.rtc ? v.started : v.sessions},
                              {v.timeLeft, v.rtc ? v.dailyAvg : v.avgSession, v.rtc ? v.finish : nullptr}};
  const char* labels[2][3] = {
      {tr(STR_STATS_TIME_LBL), tr(STR_STATS_PAGES_PER_MIN), v.rtc ? v.startedLabel : tr(STR_STATS_SESSIONS_LBL)},
      {tr(STR_TIME_LEFT_SHORT), v.rtc ? tr(STR_STATS_DAILY_AVG_LBL) : tr(STR_STATS_AVG_SESSION_LBL),
       v.rtc ? v.finishLabel : nullptr}};

  const int x0 = coverRect.x;
  const int x1 = contentRightEdge(renderer);
  const int blockH = statsBlockHeight(renderer);
  const int cellH = blockH + 4;
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int top = gardenTableTop(renderer, coverRect);
  const float cellW = static_cast<float>(x1 - x0) / 3.0f;
  renderer.fillRect(x0, top, x1 - x0 + 1, 2, true);
  for (int r = 0; r < 2; ++r) {
    const int rowTop = top + 4 + r * (cellH + 2);
    for (int k = 0; k < 3; ++k) {
      const int cellLeft = x0 + static_cast<int>(cellW * k);
      const int mid = x0 + static_cast<int>(cellW * k + cellW / 2);
      if (k > 0) renderer.drawLine(cellLeft, rowTop + 4, cellLeft, rowTop + blockH - 2, true);
      if (values[r][k] == nullptr) continue;
      const int maxW = static_cast<int>(cellW) - 6;
      const std::string value =
          renderer.truncatedText(UI_12_FONT_ID, values[r][k], maxW, EpdFontFamily::BOLD);
      const std::string label = renderer.truncatedText(SMALL_FONT_ID, labels[r][k], maxW);
      const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value.c_str(), EpdFontFamily::BOLD);
      const int labelW = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
      renderer.drawText(UI_12_FONT_ID, mid - valueW / 2, rowTop, value.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, mid - labelW / 2, rowTop + valueLineH + kStatsValueLabelGap, label.c_str(),
                        true);
    }
    if (r == 0) renderer.drawLine(x0, rowTop + cellH, x1, rowTop + cellH, true);
  }
  const int bottom = top + gardenTableHeight(renderer) - 2;
  renderer.fillRect(x0, bottom, x1 - x0 + 1, 2, true);
}

void drawGardenHome(GfxRenderer& renderer, const Rect& rect, const std::vector<RecentBook>& recentBooks,
                    bool& coverRendered, bool& coverBufferStored, const std::function<bool()>& storeCoverBuffer,
                    const BookReadingStats* stats, const float progressPercent, const GlobalReadingStats* globalStats,
                    const char* currentChapterTitle) {
  const Rect coverRect = gardenCoverRect(renderer, rect);
  if (recentBooks.empty()) {
    renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }
  const RecentBook& book = recentBooks[0];
  if (!coverRendered) {
    // The plant only changes with progress, so it is drawn once with the cover and kept in the
    // cached cover buffer; later frames restore it instead of growing it again.
    drawBookCover(renderer, coverRect, book, Color::White);
    drawGardenColumn(renderer, coverRect, book, progressPercent);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
  TouchRegistry::getInstance().add(coverRect, 0, TouchRegistry::Cover);

  const bool showStats = showBookStatsForPath(book.path);
  const int tableTop = gardenTableTop(renderer, coverRect);
  drawGardenBookText(renderer, coverRect, book, currentChapterTitle,
                     showStats ? tableTop - kGardenTableTextGap : footerCenterY(renderer, coverRect) - 60);
  if (showStats) drawGardenStatsTable(renderer, coverRect, stats, progressPercent);
  if (SETTINGS.shouldTrackReadingStats()) drawFooterStats(renderer, coverRect, globalStats);
}
}  // namespace

void DashboardTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                         const BookReadingStats* stats, const float progressPercent,
                                         const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)selectorIndex;
  (void)bufferRestored;

  if (layout_ == Layout::Garden) {
    drawGardenHome(renderer, rect, recentBooks, coverRendered, coverBufferStored, storeCoverBuffer, stats,
                   progressPercent, globalStats, currentChapterTitle);
    return;
  }

  const Rect coverRect = coverRectForScreen(renderer, rect);
  if (recentBooks.empty()) {
    renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  if (!coverRendered) {
    drawBookCover(renderer, coverRect, recentBooks[0], Color::White);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
  TouchRegistry::getInstance().add(coverRect, 0, TouchRegistry::Cover);

  drawDashboardStats(renderer, coverRect, stats, progressPercent, showBookStatsForPath(recentBooks[0].path));
  drawBookText(renderer, coverRect, recentBooks[0], currentChapterTitle);
  if (SETTINGS.shouldTrackReadingStats()) drawFooterStats(renderer, coverRect, globalStats);
}

void DashboardTheme::drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                     const GlobalReadingStats* globalStats, const float progressPercent,
                                     const char* currentChapterTitle, const bool inverted) const {
  renderer.clearScreen(inverted ? 0xFF : 0x00);

  const Rect contentRect{0, DashboardMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                         DashboardMetrics::values.homeCoverTileHeight};
  const Rect coverRect = coverRectForScreen(renderer, contentRect);
  drawBookCover(renderer, coverRect, book, inverted ? Color::White : Color::Black);
  drawDashboardStats(renderer, coverRect, stats, progressPercent, showBookStatsForPath(book.path), inverted);
  drawBookText(renderer, coverRect, book, currentChapterTitle, inverted);
  if (SETTINGS.shouldTrackReadingStats()) drawFooterStats(renderer, coverRect, globalStats, !inverted);
}
