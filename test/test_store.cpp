// core/store: recording hours left, the 20 % warning, which sessions to delete.
#include <cmath>
#include <cstring>

#include "core/store.h"
#include "tmini.h"

TEST(store_hours_left_and_warning) {
  lt::StorePlan sp;
  sp.setSpace(1800000000ll, 1800000000ll);  // XTSD 2 GB: about 1.8 GB usable
  CHECK(sp.bytesPerHour() == 72000000);
  CHECKF(std::fabs(sp.hoursLeft() - 25.0f) < 1e-4f, "%.4f h", static_cast<double>(sp.hoursLeft()));
  CHECK(!sp.low());
  sp.setSpace(1800000000ll, 360000000ll);  // exactly 20 % free: no warning yet
  CHECK(!sp.low() && std::fabs(sp.freeFraction() - 0.2f) < 1e-6f);
  sp.setSpace(1800000000ll, 359999999ll);
  CHECK(sp.low());
  // the measured rate counts after 10 minutes of recording
  sp.setRecorded(9000000, 300000);
  CHECK(sp.bytesPerHour() == 72000000);
  sp.setRecorded(18000000, 600000);  // 18 MB in 10 min = 108 MB/h
  CHECK(sp.bytesPerHour() == 108000000);
  CHECK(std::fabs(sp.hoursLeft() - 359999999.0f / 108000000.0f) < 1e-4f);
  sp.setSpace(1800000000ll, 100000000ll);
  CHECK(sp.shortfallFor(2.0f) == 216000000 - 100000000);
  CHECK(sp.shortfallFor(0.5f) == 0 && sp.shortfallFor(-1.0f) == 0);
  lt::StorePlan none;  // no card
  CHECK(none.hoursLeft() == 0.0f && !none.low() && none.freeFraction() == 0.0f);
  none.setSpace(1000, 5000);  // free larger than total: clamped
  CHECK(none.freeBytes() == 1000);
}

TEST(store_delete_plan_downloaded_oldest_first) {
  const int64_t MB = 1000000;
  const lt::StoreSession s[] = {
      {"20261031_135000", 500 * MB, true, false},   // 0
      {"20261017_092600", 300 * MB, true, false},   // 1 oldest downloaded
      {"20261108_103000", 200 * MB, true, true},    // 2 being recorded: never
      {"20261030_135600", 400 * MB, false, false},  // 3 not downloaded
      {"20261020_080000", 100 * MB, false, false},  // 4 not downloaded, older
      {"20261101_120000", 0, true, false},          // 5 empty: frees nothing
      {"20261025_100000", 250 * MB, true, false},   // 6
  };
  const int n = static_cast<int>(sizeof s / sizeof s[0]);
  lt::StorePlan sp;
  int order[16];
  lt::DeletePlan p = sp.plan(s, n, 400 * MB, order, 16);
  CHECK(p.enough && p.count == 2 && order[0] == 1 && order[1] == 6 && p.freedBytes == 550 * MB);
  CHECK(p.undownloaded == 2 && p.blocking == 0 && !p.impossible);
  p = sp.plan(s, n, 300 * MB, order, 16);
  CHECK(p.enough && p.count == 1 && order[0] == 1);
  // more than the downloaded ones hold: the undownloaded ones are only counted (oldest first)
  p = sp.plan(s, n, 1200 * MB, order, 16);
  CHECK(!p.enough && p.count == 3 && order[0] == 1 && order[1] == 6 && order[2] == 0 && p.freedBytes == 1050 * MB);
  CHECK(p.blocking == 2 && !p.impossible);
  p = sp.plan(s, n, 1150 * MB, order, 16);
  CHECK(!p.enough && p.blocking == 1);  // 100 MB more: the oldest undownloaded one would do
  p = sp.plan(s, n, 2000 * MB, order, 16);
  CHECK(!p.enough && p.count == 3 && p.blocking == 2 && p.impossible);
  for (int i = 0; i < p.count; ++i) CHECK(!s[order[i]].active && s[order[i]].downloaded);
  p = sp.plan(s, n, 0, order, 16);
  CHECK(p.enough && p.count == 0);
  p = sp.plan(s, n, 400 * MB, order, 1);  // room for one index only
  CHECK(p.count == 1 && order[0] == 1 && !p.enough);
  p = sp.plan(nullptr, 0, 10, order, 16);
  CHECK(!p.enough && p.impossible && p.count == 0);
}

TEST(store_download_markers) {
  const char* id = "20261108_103000";
  CHECK(lt::StorePlan::isDownloadMarker(".dl_20261108_103000_sodegaura.csv", id, "sodegaura"));
  CHECK(lt::StorePlan::isDownloadMarker(".dl_20261108_103000_sodegaura.vbo", id, "sodegaura"));
  CHECK(lt::StorePlan::isDownloadMarker(".dl_report.html", id, "sodegaura"));
  CHECK(lt::StorePlan::isDownloadMarker(".dl_20261108_103000_street.csv", id, nullptr));
  CHECK(!lt::StorePlan::isDownloadMarker(".dl_20261108_103000_sodegaura_imu.csv", id, "sodegaura"));
  CHECK(!lt::StorePlan::isDownloadMarker(".dl_20261108_103000_sodegaura_laps.csv", id, "sodegaura"));
  CHECK(!lt::StorePlan::isDownloadMarker(".dl_20261108_103000_sodegaura_raw.ubx", id, "sodegaura"));
  CHECK(!lt::StorePlan::isDownloadMarker("20261108_103000_sodegaura.csv", id, "sodegaura"));
  CHECK(!lt::StorePlan::isDownloadMarker(".dl_20261108_103000_sodegaura.csv", id, "fuji"));
  CHECK(!lt::StorePlan::isDownloadMarker(".dl_20261108_103001_sodegaura.csv", id, "sodegaura"));
  CHECK(!lt::StorePlan::isDownloadMarker(".dl_report.html", nullptr, "sodegaura"));
}
