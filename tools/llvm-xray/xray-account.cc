//===- xray-account.h - XRay Function Call Accounting ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements basic function call accounting from an XRay trace.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <cassert>
#include <numeric>
#include <system_error>
#include <utility>

#include "xray-account.h"
#include "xray-registry.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/XRay/InstrumentationMap.h"
#include "llvm/XRay/Trace.h"

using namespace llvm;
using namespace llvm::xray;

static cl::SubCommand Account("account", "Function call accounting");
static cl::opt<std::string> AccountInput(cl::Positional,
                                         cl::desc("<xray log file>"),
                                         cl::Required, cl::sub(Account));
static cl::opt<bool>
    AccountKeepGoing("keep-going", cl::desc("Keep going on errors encountered"),
                     cl::sub(Account), cl::init(false));
static cl::alias AccountKeepGoing2("k", cl::aliasopt(AccountKeepGoing),
                                   cl::desc("Alias for -keep_going"),
                                   cl::sub(Account));
static cl::opt<bool> AccountDeduceSiblingCalls(
    "deduce-sibling-calls",
    cl::desc("Deduce sibling calls when unrolling function call stacks"),
    cl::sub(Account), cl::init(false));
static cl::alias
    AccountDeduceSiblingCalls2("d", cl::aliasopt(AccountDeduceSiblingCalls),
                               cl::desc("Alias for -deduce_sibling_calls"),
                               cl::sub(Account));
static cl::opt<std::string>
    AccountOutput("output", cl::value_desc("output file"), cl::init("-"),
                  cl::desc("output file; use '-' for stdout"),
                  cl::sub(Account));
static cl::alias AccountOutput2("o", cl::aliasopt(AccountOutput),
                                cl::desc("Alias for -output"),
                                cl::sub(Account));
enum class AccountOutputFormats { TEXT, CSV };
static cl::opt<AccountOutputFormats>
    AccountOutputFormat("format", cl::desc("output format"),
                        cl::values(clEnumValN(AccountOutputFormats::TEXT,
                                              "text", "report stats in text"),
                                   clEnumValN(AccountOutputFormats::CSV, "csv",
                                              "report stats in csv")),
                        cl::sub(Account));
static cl::alias AccountOutputFormat2("f", cl::desc("Alias of -format"),
                                      cl::aliasopt(AccountOutputFormat),
                                      cl::sub(Account));

enum class SortField {
  FUNCID,
  COUNT,
  MIN,
  MED,
  PCT90,
  PCT99,
  MAX,
  SUM,
  FUNC,
};

static cl::opt<SortField> AccountSortOutput(
    "sort", cl::desc("sort output by this field"), cl::value_desc("field"),
    cl::sub(Account), cl::init(SortField::FUNCID),
    cl::values(clEnumValN(SortField::FUNCID, "funcid", "function id"),
               clEnumValN(SortField::COUNT, "count", "funciton call counts"),
               clEnumValN(SortField::MIN, "min", "minimum function durations"),
               clEnumValN(SortField::MED, "med", "median function durations"),
               clEnumValN(SortField::PCT90, "90p", "90th percentile durations"),
               clEnumValN(SortField::PCT99, "99p", "99th percentile durations"),
               clEnumValN(SortField::MAX, "max", "maximum function durations"),
               clEnumValN(SortField::SUM, "sum", "sum of call durations"),
               clEnumValN(SortField::FUNC, "func", "function names")));
static cl::alias AccountSortOutput2("s", cl::aliasopt(AccountSortOutput),
                                    cl::desc("Alias for -sort"),
                                    cl::sub(Account));

enum class SortDirection {
  ASCENDING,
  DESCENDING,
};
static cl::opt<SortDirection> AccountSortOrder(
    "sortorder", cl::desc("sort ordering"), cl::init(SortDirection::ASCENDING),
    cl::values(clEnumValN(SortDirection::ASCENDING, "asc", "ascending"),
               clEnumValN(SortDirection::DESCENDING, "dsc", "descending")),
    cl::sub(Account));
static cl::alias AccountSortOrder2("r", cl::aliasopt(AccountSortOrder),
                                   cl::desc("Alias for -sortorder"),
                                   cl::sub(Account));

static cl::opt<int> AccountTop("top", cl::desc("only show the top N results"),
                               cl::value_desc("N"), cl::sub(Account),
                               cl::init(-1));
static cl::alias AccountTop2("p", cl::desc("Alias for -top"),
                             cl::aliasopt(AccountTop), cl::sub(Account));

static cl::opt<std::string>
    AccountInstrMap("instr_map",
                    cl::desc("binary with the instrumentation map, or "
                             "a separate instrumentation map"),
                    cl::value_desc("binary with xray_instr_map"),
                    cl::sub(Account), cl::init(""));
static cl::alias AccountInstrMap2("m", cl::aliasopt(AccountInstrMap),
                                  cl::desc("Alias for -instr_map"),
                                  cl::sub(Account));

namespace {

template <class T, class U> void setMinMax(std::pair<T, T> &MM, U &&V) {
  if (MM.first == 0 || MM.second == 0)
    MM = std::make_pair(std::forward<U>(V), std::forward<U>(V));
  else
    MM = std::make_pair(std::min(MM.first, V), std::max(MM.second, V));
}

template <class T> T diff(T L, T R) { return std::max(L, R) - std::min(L, R); }

} // namespace

bool LatencyAccountant::accountRecord(const XRayRecord &Record) {
  setMinMax(PerThreadMinMaxTSC[Record.TId], Record.TSC);
  setMinMax(PerCPUMinMaxTSC[Record.CPU], Record.TSC);

  if (CurrentMaxTSC == 0)
    CurrentMaxTSC = Record.TSC;

  if (Record.TSC < CurrentMaxTSC)
    return false;

  auto &ThreadStack = PerThreadFunctionStack[Record.TId];
  switch (Record.Type) {
  case RecordTypes::ENTER: {
    // Function Enter
    ThreadStack.emplace_back(Record.FuncId, Record.TSC);
    break;
  }
  case RecordTypes::EXIT: {
    // Function Exit
    if (ThreadStack.back().first == Record.FuncId) {
      const auto &Top = ThreadStack.back();
      recordLatency(Top.first, diff(Top.second, Record.TSC));
      ThreadStack.pop_back();
      break;
    }

    if (!DeduceSiblingCalls)
      return false;

    // Look for the parent up the stack.
    auto Parent =
        std::find_if(ThreadStack.rbegin(), ThreadStack.rend(),
                     [&](const std::pair<const int32_t, uint64_t> &E) {
                       return E.first == Record.FuncId;
                     });
    if (Parent == ThreadStack.rend())
      return false;

    // Account time for this apparently sibling call exit up the stack.
    // Considering the following case:
    //
    //   f()
    //    g()
    //      h()
    //
    // We might only ever see the following entries:
    //
    //   -> f()
    //   -> g()
    //   -> h()
    //   <- h()
    //   <- f()
    //
    // Now we don't see the exit to g() because some older version of the XRay
    // runtime wasn't instrumenting tail exits. If we don't deduce tail calls,
    // we may potentially never account time for g() -- and this code would have
    // already bailed out, because `<- f()` doesn't match the current "top" of
    // stack where we're waiting for the exit to `g()` instead. This is not
    // ideal and brittle -- so instead we provide a potentially inaccurate
    // accounting of g() instead, computing it from the exit of f().
    //
    // While it might be better that we account the time between `-> g()` and
    // `-> h()` as the proper accounting of time for g() here, this introduces
    // complexity to do correctly (need to backtrack, etc.).
    //
    // FIXME: Potentially implement the more complex deduction algorithm?
    auto I = std::next(Parent).base();
    for (auto &E : make_range(I, ThreadStack.end())) {
      recordLatency(E.first, diff(E.second, Record.TSC));
    }
    ThreadStack.erase(I, ThreadStack.end());
    break;
  }
  }

  return true;
}

namespace {

// We consolidate the data into a struct which we can output in various forms.
struct ResultRow {
  uint64_t Count;
  double Min;
  double Median;
  double Pct90;
  double Pct99;
  double Max;
  double Sum;
  std::string DebugInfo;
  std::string Function;
};

ResultRow getStats(std::vector<uint64_t> &Timings) {
  assert(!Timings.empty());
  ResultRow R;
  R.Sum = std::accumulate(Timings.begin(), Timings.end(), 0.0);
  auto MinMax = std::minmax_element(Timings.begin(), Timings.end());
  R.Min = *MinMax.first;
  R.Max = *MinMax.second;
  auto MedianOff = Timings.size() / 2;
  std::nth_element(Timings.begin(), Timings.begin() + MedianOff, Timings.end());
  R.Median = Timings[MedianOff];
  auto Pct90Off = std::floor(Timings.size() * 0.9);
  std::nth_element(Timings.begin(), Timings.begin() + Pct90Off, Timings.end());
  R.Pct90 = Timings[Pct90Off];
  auto Pct99Off = std::floor(Timings.size() * 0.99);
  std::nth_element(Timings.begin(), Timings.begin() + Pct90Off, Timings.end());
  R.Pct99 = Timings[Pct99Off];
  R.Count = Timings.size();
  return R;
}

} // namespace

template <class F>
void LatencyAccountant::exportStats(const XRayFileHeader &Header, F Fn) const {
  using TupleType = std::tuple<int32_t, uint64_t, ResultRow>;
  std::vector<TupleType> Results;
  Results.reserve(FunctionLatencies.size());
  for (auto FT : FunctionLatencies) {
    const auto &FuncId = FT.first;
    auto &Timings = FT.second;
    Results.emplace_back(FuncId, Timings.size(), getStats(Timings));
    auto &Row = std::get<2>(Results.back());
    if (Header.CycleFrequency) {
      double CycleFrequency = Header.CycleFrequency;
      Row.Min /= CycleFrequency;
      Row.Median /= CycleFrequency;
      Row.Pct90 /= CycleFrequency;
      Row.Pct99 /= CycleFrequency;
      Row.Max /= CycleFrequency;
      Row.Sum /= CycleFrequency;
    }

    Row.Function = FuncIdHelper.SymbolOrNumber(FuncId);
    Row.DebugInfo = FuncIdHelper.FileLineAndColumn(FuncId);
  }

  // Sort the data according to user-provided flags.
  switch (AccountSortOutput) {
  case SortField::FUNCID:
    std::sort(Results.begin(), Results.end(),
              [](const TupleType &L, const TupleType &R) {
                if (AccountSortOrder == SortDirection::ASCENDING)
                  return std::get<0>(L) < std::get<0>(R);
                if (AccountSortOrder == SortDirection::DESCENDING)
                  return std::get<0>(L) > std::get<0>(R);
                llvm_unreachable("Unknown sort direction");
              });
    break;
  case SortField::COUNT:
    std::sort(Results.begin(), Results.end(),
              [](const TupleType &L, const TupleType &R) {
                if (AccountSortOrder == SortDirection::ASCENDING)
                  return std::get<1>(L) < std::get<1>(R);
                if (AccountSortOrder == SortDirection::DESCENDING)
                  return std::get<1>(L) > std::get<1>(R);
                llvm_unreachable("Unknown sort direction");
              });
    break;
  default:
    // Here we need to look into the ResultRow for the rest of the data that
    // we want to sort by.
    std::sort(Results.begin(), Results.end(),
              [&](const TupleType &L, const TupleType &R) {
                auto &LR = std::get<2>(L);
                auto &RR = std::get<2>(R);
                switch (AccountSortOutput) {
                case SortField::COUNT:
                  if (AccountSortOrder == SortDirection::ASCENDING)
                    return LR.Count < RR.Count;
                  if (AccountSortOrder == SortDirection::DESCENDING)
                    return LR.Count > RR.Count;
                  llvm_unreachable("Unknown sort direction");
                case SortField::MIN:
                  if (AccountSortOrder == SortDirection::ASCENDING)
                    return LR.Min < RR.Min;
                  if (AccountSortOrder == SortDirection::DESCENDING)
                    return LR.Min > RR.Min;
                  llvm_unreachable("Unknown sort direction");
                case SortField::MED:
                  if (AccountSortOrder == SortDirection::ASCENDING)
                    return LR.Median < RR.Median;
                  if (AccountSortOrder == SortDirection::DESCENDING)
                    return LR.Median > RR.Median;
                  llvm_unreachable("Unknown sort direction");
                case SortField::PCT90:
                  if (AccountSortOrder == SortDirection::ASCENDING)
                    return LR.Pct90 < RR.Pct90;
                  if (AccountSortOrder == SortDirection::DESCENDING)
                    return LR.Pct90 > RR.Pct90;
                  llvm_unreachable("Unknown sort direction");
                case SortField::PCT99:
                  if (AccountSortOrder == SortDirection::ASCENDING)
                    return LR.Pct99 < RR.Pct99;
                  if (AccountSortOrder == SortDirection::DESCENDING)
                    return LR.Pct99 > RR.Pct99;
                  llvm_unreachable("Unknown sort direction");
                case SortField::MAX:
                  if (AccountSortOrder == SortDirection::ASCENDING)
                    return LR.Max < RR.Max;
                  if (AccountSortOrder == SortDirection::DESCENDING)
                    return LR.Max > RR.Max;
                  llvm_unreachable("Unknown sort direction");
                case SortField::SUM:
                  if (AccountSortOrder == SortDirection::ASCENDING)
                    return LR.Sum < RR.Sum;
                  if (AccountSortOrder == SortDirection::DESCENDING)
                    return LR.Sum > RR.Sum;
                  llvm_unreachable("Unknown sort direction");
                default:
                  llvm_unreachable("Unsupported sort order");
                }
              });
    break;
  }

  if (AccountTop > 0)
    Results.erase(Results.begin() + AccountTop.getValue(), Results.end());

  for (const auto &R : Results)
    Fn(std::get<0>(R), std::get<1>(R), std::get<2>(R));
}

void LatencyAccountant::exportStatsAsText(raw_ostream &OS,
                                          const XRayFileHeader &Header) const {
  OS << "Functions with latencies: " << FunctionLatencies.size() << "\n";

  // We spend some effort to make the text output more readable, so we do the
  // following formatting decisions for each of the fields:
  //
  //   - funcid: 32-bit, but we can determine the largest number and be
  //   between
  //     a minimum of 5 characters, up to 9 characters, right aligned.
  //   - count:  64-bit, but we can determine the largest number and be
  //   between
  //     a minimum of 5 characters, up to 9 characters, right aligned.
  //   - min, median, 90pct, 99pct, max: double precision, but we want to keep
  //     the values in seconds, with microsecond precision (0.000'001), so we
  //     have at most 6 significant digits, with the whole number part to be
  //     at
  //     least 1 character. For readability we'll right-align, with full 9
  //     characters each.
  //   - debug info, function name: we format this as a concatenation of the
  //     debug info and the function name.
  //
  static constexpr char StatsHeaderFormat[] =
      "{0,+9} {1,+10} [{2,+9}, {3,+9}, {4,+9}, {5,+9}, {6,+9}] {7,+9}";
  static constexpr char StatsFormat[] =
      R"({0,+9} {1,+10} [{2,+9:f6}, {3,+9:f6}, {4,+9:f6}, {5,+9:f6}, {6,+9:f6}] {7,+9:f6})";
  OS << llvm::formatv(StatsHeaderFormat, "funcid", "count", "min", "med", "90p",
                      "99p", "max", "sum")
     << llvm::formatv("  {0,-12}\n", "function");
  exportStats(Header, [&](int32_t FuncId, size_t Count, const ResultRow &Row) {
    OS << llvm::formatv(StatsFormat, FuncId, Count, Row.Min, Row.Median,
                        Row.Pct90, Row.Pct99, Row.Max, Row.Sum)
       << "  " << Row.DebugInfo << ": " << Row.Function << "\n";
  });
}

void LatencyAccountant::exportStatsAsCSV(raw_ostream &OS,
                                         const XRayFileHeader &Header) const {
  OS << "funcid,count,min,median,90%ile,99%ile,max,sum,debug,function\n";
  exportStats(Header, [&](int32_t FuncId, size_t Count, const ResultRow &Row) {
    OS << FuncId << ',' << Count << ',' << Row.Min << ',' << Row.Median << ','
       << Row.Pct90 << ',' << Row.Pct99 << ',' << Row.Max << "," << Row.Sum
       << ",\"" << Row.DebugInfo << "\",\"" << Row.Function << "\"\n";
  });
}

using namespace llvm::xray;

static CommandRegistration Unused(&Account, []() -> Error {
  InstrumentationMap Map;
  if (!AccountInstrMap.empty()) {
    auto InstrumentationMapOrError = loadInstrumentationMap(AccountInstrMap);
    if (!InstrumentationMapOrError)
      return joinErrors(make_error<StringError>(
                            Twine("Cannot open instrumentation map '") +
                                AccountInstrMap + "'",
                            std::make_error_code(std::errc::invalid_argument)),
                        InstrumentationMapOrError.takeError());
    Map = std::move(*InstrumentationMapOrError);
  }

  std::error_code EC;
  raw_fd_ostream OS(AccountOutput, EC, sys::fs::OpenFlags::F_Text);
  if (EC)
    return make_error<StringError>(
        Twine("Cannot open file '") + AccountOutput + "' for writing.", EC);

  const auto &FunctionAddresses = Map.getFunctionAddresses();
  symbolize::LLVMSymbolizer::Options Opts(
      symbolize::FunctionNameKind::LinkageName, true, true, false, "");
  symbolize::LLVMSymbolizer Symbolizer(Opts);
  llvm::xray::FuncIdConversionHelper FuncIdHelper(AccountInstrMap, Symbolizer,
                                                  FunctionAddresses);
  xray::LatencyAccountant FCA(FuncIdHelper, AccountDeduceSiblingCalls);
  auto TraceOrErr = loadTraceFile(AccountInput);
  if (!TraceOrErr)
    return joinErrors(
        make_error<StringError>(
            Twine("Failed loading input file '") + AccountInput + "'",
            std::make_error_code(std::errc::executable_format_error)),
        TraceOrErr.takeError());

  auto &T = *TraceOrErr;
  for (const auto &Record : T) {
    if (FCA.accountRecord(Record))
      continue;
    for (const auto &ThreadStack : FCA.getPerThreadFunctionStack()) {
      errs() << "Thread ID: " << ThreadStack.first << "\n";
      auto Level = ThreadStack.second.size();
      for (const auto &Entry : llvm::reverse(ThreadStack.second))
        errs() << "#" << Level-- << "\t"
               << FuncIdHelper.SymbolOrNumber(Entry.first) << '\n';
    }
    if (!AccountKeepGoing)
      return make_error<StringError>(
          Twine("Failed accounting function calls in file '") + AccountInput +
              "'.",
          std::make_error_code(std::errc::executable_format_error));
  }
  switch (AccountOutputFormat) {
  case AccountOutputFormats::TEXT:
    FCA.exportStatsAsText(OS, T.getFileHeader());
    break;
  case AccountOutputFormats::CSV:
    FCA.exportStatsAsCSV(OS, T.getFileHeader());
    break;
  }

  return Error::success();
});
