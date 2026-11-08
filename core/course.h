// core/course.h - registered courses (/config/courses.json, ARCH §5.4): the automatic choice by
// position (course centre within 3 km), lt::CourseDef for the lap timer, and courses made from the
// track definition of a RaceChrono VBO export (調査 §2.4, D13).
//
// courses.json:
//   {"version":1,"courses":[{"id":"sodegaura","name":"袖ヶ浦フォレストレースウェイ","min_lap_s":30,
//     "gates":[{"type":"sf","lat":35.3949090,"lon":140.0880580,"heading":31.1,"width":50},
//              {"type":"split","name":"Split 1","lat":35.3922588,"lon":140.0909627,"heading":250.1,"width":30}]}]}
//  - id: ASCII [a-z0-9_], 1..31 characters, unique. name: UTF-8 (missing: the id).
//  - gates: exactly one "sf" and up to kCourseMaxGates - 1 "split" lines; the splits are timed in the
//    order of the file (driving order). lat/lon degrees (7 decimals are kept exactly), heading = the
//    direction of travel through the line (0 = north, clockwise), width in metres (missing: 50 for
//    sf, 30 for split). min_lap_s missing: 30. "center":[lat,lon] optional, else the mean of the gates.
//  - unknown keys are ignored (e.g. "_note").
#pragma once

#include <cstdint>

#include "geo.h"
#include "json.h"
#include "lap.h"

namespace lt {

constexpr int kMaxCourses = 16;
constexpr int kCourseMaxGates = 8;       // S/F + up to 7 splits
constexpr float kNearCourseM = 3000.0f;  // "コース近辺": within 3 km of the course centre (UI設計 §2)
constexpr float kSfWidthM = 50.0f;       // RaceChrono's Start line (pit lane counts, D13)
constexpr float kSplitWidthM = 30.0f;    // RaceChrono's Split lines
constexpr float kDefaultMinLapS = 30.0f;

enum class GateType : uint8_t { Sf, Split };

struct CourseGate {
  GateType type;
  char name[24];  // UTF-8, "" when none
  int32_t latE7;
  int32_t lonE7;
  float headingDeg;  // direction of travel through the line [0, 360)
  float widthM;
};

struct Course {
  char id[32];    // ASCII [a-z0-9_]
  char name[96];  // UTF-8
  float minLapS;
  int32_t centerLatE7;  // "center" of the file, else the mean of the gates
  int32_t centerLonE7;
  bool centerGiven;
  int numGates;
  CourseGate gates[kCourseMaxGates];  // file order
};

class CourseDb {
 public:
  // Replaces the content. On failure the database is empty and err (may be null) tells why:
  // JSON syntax codes (json.h) or bad-version, missing-version, missing-courses, too-many-courses,
  // bad-id, missing-id, duplicate-id, name-too-long, bad-min-lap, bad-center, too-many-gates,
  // bad-gate-type, gate-name-too-long, bad-lat, bad-lon, bad-heading, bad-width, missing-lat,
  // missing-lon, missing-heading, missing-type, no-sf, two-sf.
  bool load(json::Source& src, json::Error* err);
  bool load(const char* text, int len, json::Error* err);
  // The same checks without keeping anything (for an uploaded file before it replaces the old one).
  static bool check(json::Source& src, json::Error* err);

  void clear() { n_ = 0; }
  int count() const { return n_; }
  const Course& course(int i) const { return c_[i]; }
  int find(const char* id) const;  // -1: not registered
  bool put(const Course& c);       // adds, or replaces the course with the same id; false: full/invalid
  bool remove(const char* id);

  // Nearest course whose centre is within maxM (-1: none). distM (may be null): its distance.
  int nearest(int32_t latE7, int32_t lonE7, float maxM = kNearCourseM, float* distM = nullptr) const;
  float distanceM(int i, int32_t latE7, int32_t lonE7) const;  // to the course centre
  bool buildCourseDef(int i, CourseDef* out) const;
  void initPlane(int i, Plane* pl) const;  // local plane with its origin at the course centre

  int toJson(char* out, int cap) const;  // courses.json text; 0 when cap is too small

  // RaceChrono VBO export -> course: [session data] "name ..." is the course name, each [laptiming]
  // line "Start|Split lonA latA lonB latB ¬ name" (minutes, longitude positive to the WEST) is a gate
  // through A with the direction of travel B -> A (Start: "sf", 50 m; Split: "split", 30 m, in the
  // order of the file); min_lap_s 30. Reading stops at [data], so the header part is enough.
  // id: ASCII id for the course; null or "": made from the ASCII letters of the name, or from the
  // S/F position ("course_n35394_e140088"). why (may be null): no-laptiming, no-start-line,
  // two-start-lines, too-many-gates, bad-laptiming-line, bad-id.
  static bool parseRaceChronoVbo(const char* text, int len, const char* id, Course* out, const char** why);
  // The same, written as a courses.json text holding that one course. 0 on an error or when cap is too small.
  static int fromRaceChronoVbo(const char* text, int len, const char* id, char* out, int cap,
                               const char** why);
  // A courses.json text of the given courses
  static int writeJson(const Course* courses, int n, char* out, int cap);

  static bool validId(const char* id);

 private:
  Course c_[kMaxCourses];
  int n_ = 0;
};

}  // namespace lt
