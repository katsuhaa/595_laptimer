// Tests of the parts ported from the round gauge (katsuhaa/abarth595-round-gauge 2.15.3, pc_test/unit_main.cpp):
// same inputs, same steps, same expected values. The round gauge's messages are kept in the comments.
#include <cstring>

#include "core/load.h"
#include "core/trip.h"
#include "core/veh.h"
#include "tmini.h"

namespace {

lt::VehValues rpmSig(int rpm, bool ok) {
  lt::VehValues x;
  std::memset(&x, 0, sizeof(x));
  x.rpmOk = ok;
  x.rpm = rpm;
  return x;
}

}  // namespace

// round gauge unit_main 「3000rpm 以上・未満の累計(trip)」 and 「走行時間・全開の時間」
TEST(round_trip_times) {
  lt::Trip trip;
  lt::VehValues r = rpmSig(0, false);
  trip.update(r, 1000);  // no signal
  r.rpmOk = true;
  r.rpm = 3000;
  trip.update(r, 1400);
  r.rpm = 2999;
  trip.update(r, 1800);
  r.rpmOk = false;
  trip.update(r, 2200);
  CHECKF(trip.aboveMs() == 400 && trip.belowMs() == 400, "3000 rpm 以上・未満 (3000 ちょうどは以上、信号なしは数えない): %u %u",
         trip.aboveMs(), trip.belowMs());
  trip.resetTimes();
  r.rpmOk = true;
  r.rpm = 3400;
  trip.update(r, 2600, 3500);
  r.rpm = 3600;
  trip.update(r, 3000, 3500);
  CHECK(trip.aboveMs() == 400 && trip.belowMs() == 400);  // 過負荷 回転数 3500: 3400 は未満、3600 は以上
  trip.resetTimes();

  lt::VehValues d;
  std::memset(&d, 0, sizeof(d));
  d.linkUp = d.speedOk = d.throttleOk = true;
  uint32_t tt = 100000;
  d.speedKmh = 0;
  d.throttlePct = 100;
  trip.update(d, tt);
  const uint32_t m0 = trip.movingMs(), w0 = trip.wotMs();
  for (int i = 0; i < 100; i++) trip.update(d, tt += 20);
  CHECK(trip.movingMs() == m0 && trip.wotMs() == w0);  // 停車中は走行時間も全開の時間も増えない
  d.speedKmh = 40;
  d.throttlePct = 50;
  for (int i = 0; i < 300; i++) trip.update(d, tt += 20);
  CHECK(trip.movingMs() == m0 + 6000 && trip.wotMs() == w0);  // 走行中(アクセル 50%)は走行時間だけ
  d.throttlePct = 96;
  for (int i = 0; i < 100; i++) trip.update(d, tt += 20);
  CHECK(trip.movingMs() == m0 + 8000 && trip.wotMs() == w0 + 2000);  // 全開(95% 以上)は両方
  d.speedOk = false;
  for (int i = 0; i < 100; i++) trip.update(d, tt += 20);
  CHECK(trip.movingMs() == m0 + 8000 && trip.wotMs() == w0 + 2000);  // 車速の信号がない間は数えない
  // a gap counts at most 500 ms
  trip.resetTimes();
  r = rpmSig(4000, true);
  trip.update(r, tt += 20);    // 20 ms since the last call
  trip.update(r, tt += 5000);  // a 5 s gap counts 500 ms
  CHECKF(trip.aboveMs() == 520, "%u", trip.aboveMs());
  // maintenance (ui_spec 7): ▲ reached the hours
  CHECK(!lt::tripMaintDue(5 * 3600000u - 1, 5) && lt::tripMaintDue(5 * 3600000u, 5) && !lt::tripMaintDue(99 * 3600000u, 0));
  CHECK(lt::tripMaintLeftMs(3600000u, 5) == 4 * 3600000u && lt::tripMaintLeftMs(6 * 3600000u, 5) == 0);
}

// round gauge unit_main 「29. 2.10.0: 高負荷時間」 (20 ms steps)
TEST(round_load_rules) {
  lt::LoadSet lset;
  lt::Load load;
  uint32_t T = 1000;
  auto run = [&](int rpm, uint32_t ms, bool ok) {
    for (uint32_t k = 0; k < ms; k += 20) load.update(rpmSig(rpm, ok), lset, T += 20);
  };
  load.testReset();
  load.update(rpmSig(3000, true), lset, T);
  run(3000, 60000, true);
  CHECK(load.valueMs() / 1000 == 60);  // 3000 rpm 以上を 60 秒で 1:00 増える
  run(2999, 30000, true);
  CHECK(load.valueMs() / 1000 == 30);  // 2999 rpm を 30 秒で 0:30 減る
  run(0, 30000, true);
  CHECK(load.valueMs() == 0);  // 0 rpm も同じく減る
  run(0, 10000, true);
  CHECK(load.valueMs() == 0);  // 0 より下にならない
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 600000, true);
  run(2999, 299000, true);
  CHECK(load.valueMs() > 0);  // リセット: 3000 rpm 未満が 4:59 では 0 にならない
  run(2999, 1020, true);
  CHECK(load.valueMs() == 0);  // リセット: 5:00 で 0
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 600000, true);
  run(3000, 400000, true);
  CHECK(load.valueMs() == 1000000);  // 3000 rpm ちょうどは過負荷(増える、リセットも数えない)
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 900000, true);
  run(1000, 240000, true);
  run(3000, 1000, true);
  run(1000, 240000, true);
  CHECK(load.valueMs() > 0);  // リセット: 途中で 3000 rpm 以上になったら数え直し
  lset.loadResetMin = 1;
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 600000, true);
  run(1000, 59000, true);
  CHECK(load.valueMs() > 0);  // リセット 1 分: 0:59 では 0 にならない
  run(1000, 1020, true);
  CHECK(load.valueMs() == 0);  // リセット 1 分: 1:00 で 0
  lset = lt::LoadSet{};
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 120000, true);
  run(1000, 240000, true);
  const uint32_t keep = load.valueMs();
  run(0, 60000, false);
  CHECK(load.valueMs() == keep && load.lowMs(T) == 0);  // 回転数の信号なし: 値は変わらず、リセットの数えは 0 から
  // power off time (GPS time)
  load.testReset();
  load.restore(600000, 1793065338u, 0);
  load.update(rpmSig(850, true), lset, T += 20);
  CHECK(load.valueMs() == 600000);  // 保存した値から続ける
  load.onTime(1793065338u + 180 + 20, lset, 20000);
  CHECK(load.valueMs() == 420000);  // 保存した時刻から 3 分後に起動: 3 分減る
  load.onTime(1793065338u + 900, lset, 30000);
  CHECK(load.valueMs() == 420000);  // 引くのは 1 回だけ
  load.testReset();
  load.restore(600000, 1793065338u, 120000);
  load.onTime(1793065338u + 180 + 20, lset, 20000);
  CHECK(load.valueMs() == 0);  // 未満の経過 2 分 + 切れていた 3 分 で 0
  load.testReset();
  load.restore(600000, 0, 0);
  load.onTime(1793065338u, lset, 20000);
  CHECK(load.valueMs() == 600000);  // 前回の起動で時刻が分からなかったときは引かない
  load.testReset();
  load.restore(600000, 1793065338u, 0);
  load.onTime(1793065338u - 100, lset, 20000);
  CHECK(load.valueMs() == 600000);  // 差が負のときは引かない
  // colour (yellow 10, red 30)
  load.testSet(9 * 60000 + 59000, false);
  CHECK(load.level(lset) == 0);
  load.testSet(10 * 60000, false);
  CHECK(load.level(lset) == 1);
  load.testSet(30 * 60000, false);
  CHECK(load.level(lset) == 2);
  load.testSet(29 * 60000 + 59000, false);
  CHECK(load.level(lset) == 1);
  lset.loadYellowMin = 0;
  load.testSet(29 * 60000, false);
  CHECK(load.level(lset) == 0 && lt::Load::enabled(lset));  // 黄 0: 白 → 赤
  load.testSet(30 * 60000, false);
  CHECK(load.level(lset) == 2);
  lset = lt::LoadSet{};
  lset.loadRedMin = 0;
  load.testSet(40 * 60000, false);
  CHECK(load.level(lset) == 1);  // 赤 0: 白 → 黄
  lset.loadYellowMin = 0;
  CHECK(!lt::Load::enabled(lset));  // 黄・赤とも 0 で機能を切
  load.update(rpmSig(5000, true), lset, T += 20);
  CHECK(load.valueMs() == 0);  // 機能が切: 数えず 0
  lset = lt::LoadSet{};
  // cool-down
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 599000, true);
  run(1000, 60000, true);
  CHECK(!load.coolDown(T) && load.valueMs() > 0);  // 黄(10:00)に届いていなければ出さない
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 600000, true);
  CHECK(load.level(lset) == 1 && !load.coolDown(T));  // 黄でも 3000 rpm 以上の間は出さない
  run(1000, 59000, true);
  CHECK(!load.coolDown(T));  // 0:59 ではまだ
  run(1000, 1020, true);
  CHECKF(load.coolDown(T) && load.coolLeftMs(lset, T) == 240000 && load.level(lset) == 0, "1:00 で出す、残り 4:00 (%u)",
         load.coolLeftMs(lset, T));
  run(3000, 1000, true);
  CHECK(!load.coolDown(T) && load.lowMs(T) == 0);  // 3000 rpm 以上で過負荷に戻る
  run(2999, 60020, true);
  CHECK(load.coolDown(T));  // また 1 分続いたら出す
  run(2999, 240000, true);
  CHECK(load.valueMs() == 0 && !load.coolDown(T));  // リセットまでの残りが 0 で 0 にして消す
  run(5000, 60000, true);
  run(1000, 61000, true);
  CHECK(!load.coolDown(T));  // 0 になったら黄になったことは忘れる
  lset.loadYellowMin = 0;
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 1799000, true);
  run(1000, 61000, true);
  CHECK(!load.coolDown(T));  // 黄 0: 赤(30:00)に届いていなければ出さない
  load.testReset();
  load.update(rpmSig(5000, true), lset, T);
  run(5000, 1800000, true);
  run(1000, 61000, true);
  CHECK(load.coolDown(T));  // 黄 0: 赤になっていたら出す
  lset = lt::LoadSet{};
  load.testSet(3 * 60000, true);
  load.update(rpmSig(1000, true), lset, T += 20);
  run(1000, 61000, true);
  CHECK(load.coolDown(T));
  // 累積時間 リセット: the overload time is capped
  load.testSet(49 * 60000 + 33000, true);
  load.capMs(3600000);
  CHECK(load.valueMs() == 49 * 60000 + 33000);
  load.capMs(1800000);
  CHECK(load.valueMs() == 1800000);
  load.capMs(0);
  CHECK(load.valueMs() == 0 && !load.warned(lset));
}
