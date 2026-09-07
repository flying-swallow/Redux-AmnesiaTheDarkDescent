#include "resources/FileSearcher.h"
#include "system/Platform.h"

#include <cstdio>
#include <map>

namespace {

struct FakeDirectory {
  hpl::tWStringVec files;
  hpl::tWStringVec folders;
};

std::map<hpl::tWString, FakeDirectory> g_fakeDirectories;

hpl::tWString Normalize(const hpl::tWString &path) {
  hpl::tWString normalized = path;
  for (wchar_t &character : normalized) {
    if (character == L'\\')
      character = L'/';
  }
  return normalized;
}

const FakeDirectory *FindFakeDirectory(const hpl::tWString &path) {
  const auto it = g_fakeDirectories.find(Normalize(path));
  return it == g_fakeDirectories.end() ? nullptr : &it->second;
}

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

void SetUpFakeTree() {
  g_fakeDirectories.clear();

  g_fakeDirectories[L"/virtual/tree"].folders = {L"textures", L"mod"};
  g_fakeDirectories[L"/virtual/tree/textures"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};
  g_fakeDirectories[L"/virtual/tree/mod"].folders = {L"textures"};
  g_fakeDirectories[L"/virtual/tree/mod/textures"].folders = {L"detail"};
  g_fakeDirectories[L"/virtual/tree/mod/textures/detail"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};

  g_fakeDirectories[L"/virtual/single"].files = {L"Only.DDS"};

  g_fakeDirectories[L"/virtual/tie/a/textures"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};
  g_fakeDirectories[L"/virtual/tie/b/textures"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};

  g_fakeDirectories[L"/virtual/priority/stock"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/priority/override"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/priority/stock/materials/textures"].files = {
      L"foo.dds"};

  g_fakeDirectories[L"/virtual/bare/stock"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/bare/override"].files = {L"foo.dds"};

  g_fakeDirectories[L"/virtual/equal/score/winner/materials"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/equal/score/loser"].files = {L"foo.dds"};

  g_fakeDirectories[L"/virtual/dup/first"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/second"].files = {L"foo.dds"};

  g_fakeDirectories[L"/virtual/dup/repeat"].folders = {L"nested"};
  g_fakeDirectories[L"/virtual/dup/repeat"].files = {L"repeat.dds"};
  g_fakeDirectories[L"/virtual/dup/repeat/nested"].files = {
      L"repeat.dds"};

  g_fakeDirectories[L"/virtual/dup/priority/override"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/priority/default"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/priority/readd"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/priority/competing"].files = {
      L"foo.dds"};

  g_fakeDirectories[L"/virtual/rescan"].files = {L"original.dds"};

  g_fakeDirectories[L"/virtual/dup/delta/first"].files = {
      L"level.map_delta"};
  g_fakeDirectories[L"/virtual/dup/delta/second"].files = {
      L"level.map_delta"};
}

bool CheckDepthAndAdjacentNameResolution() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/tree", "*", true);

  int equalCount = -1;
  const hpl::tWString &resolved = searcher.GetFilePath(
      "wanted/root/mod/textures/detail/foo.dds", &equalCount);
  return Check(resolved == L"/virtual/tree/mod/textures/detail/foo.dds",
               "the deepest matching trailing directories win") &&
         Check(equalCount == 3,
               "depth scoring counts the matching trailing directories") &&
         Check(resolved.find(L"foo.dds") != hpl::tWString::npos,
               "adjacent bare filenames never replace the queried filename");
}

bool CheckTieBreakOrder() {
  hpl::cFileSearcher firstA;
  firstA.AddDirectory(L"/virtual/tie/a/textures", "*", false);
  firstA.AddDirectory(L"/virtual/tie/b/textures", "*", false);

  int equalCount = -1;
  const hpl::tWString &aThenB = firstA.GetFilePath(
      "wanted/textures/foo.dds", &equalCount);
  if (!Check(aThenB == L"/virtual/tie/a/textures/foo.dds",
             "the first equally-scored candidate wins when A is indexed first") ||
      !Check(equalCount == 1, "equal candidates report their shared depth")) {
    return false;
  }

  hpl::cFileSearcher firstB;
  firstB.AddDirectory(L"/virtual/tie/b/textures", "*", false);
  firstB.AddDirectory(L"/virtual/tie/a/textures", "*", false);

  const hpl::tWString &bThenA = firstB.GetFilePath(
      "wanted/textures/foo.dds", &equalCount);
  return Check(bThenA == L"/virtual/tie/b/textures/foo.dds",
               "the first equally-scored candidate wins when B is indexed first");
}

bool CheckMissingAndSingleCandidate() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/single", "*", false);

  int equalCount = 99;
  const hpl::tWString &missing =
      searcher.GetFilePath("missing.dds", &equalCount);
  if (!Check(missing.empty(), "a missing bare filename resolves to empty") ||
      !Check(equalCount == 0, "a missing bare filename reports zero matches")) {
    return false;
  }

  return Check(searcher.GetFilePath("only.dds") == L"/virtual/single/Only.DDS",
               "a single candidate resolves without a path hint");
}

bool CheckHigherPriorityWinsWhenOverrideIndexedFirst() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/priority/override", "*", false, 10);
  searcher.AddDirectory(L"/virtual/priority/stock", "*", false, 0);

  const hpl::tWString &resolved = searcher.GetFilePath("wanted/foo.dds");
  return Check(resolved == L"/virtual/priority/override/foo.dds",
               "the higher-priority override wins when indexed first");
}

bool CheckHigherPriorityWinsWhenOverrideIndexedLast() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/priority/stock", "*", false, 0);
  searcher.AddDirectory(L"/virtual/priority/override", "*", false, 10);

  const hpl::tWString &resolved = searcher.GetFilePath("wanted/foo.dds");
  return Check(resolved == L"/virtual/priority/override/foo.dds",
               "the higher-priority override wins when indexed last");
}

bool CheckPriorityBeatsPathScore() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/priority/stock/materials/textures", "*",
                        false, 0);
  searcher.AddDirectory(L"/virtual/priority/override", "*", false, 10);

  int equalCount = -1;
  const hpl::tWString &resolved = searcher.GetFilePath(
      "wanted/materials/textures/foo.dds", &equalCount);
  return Check(resolved == L"/virtual/priority/override/foo.dds",
               "priority beats a better matching stock path") &&
         Check(equalCount == 0,
               "the winning priority candidate reports its path score");
}

bool CheckDefaultPriorityRegression() {
  hpl::cFileSearcher bestScore;
  bestScore.AddDirectory(L"/virtual/priority/stock/materials/textures", "*",
                         false);
  bestScore.AddDirectory(L"/virtual/priority/override", "*", false);

  int equalCount = -1;
  const hpl::tWString &resolved = bestScore.GetFilePath(
      "wanted/materials/textures/foo.dds", &equalCount);
  if (!Check(resolved == L"/virtual/priority/stock/materials/textures/foo.dds",
             "default priorities preserve the best path-score winner") ||
      !Check(equalCount == 2,
             "default-priority resolution reports the best path score")) {
    return false;
  }

  hpl::cFileSearcher firstA;
  firstA.AddDirectory(L"/virtual/tie/a/textures", "*", false);
  firstA.AddDirectory(L"/virtual/tie/b/textures", "*", false);
  const hpl::tWString &aThenB = firstA.GetFilePath("wanted/textures/foo.dds");
  if (!Check(aThenB == L"/virtual/tie/a/textures/foo.dds",
             "default-priority score ties keep the first indexed candidate")) {
    return false;
  }

  hpl::cFileSearcher firstB;
  firstB.AddDirectory(L"/virtual/tie/b/textures", "*", false);
  firstB.AddDirectory(L"/virtual/tie/a/textures", "*", false);
  const hpl::tWString &bThenA = firstB.GetFilePath("wanted/textures/foo.dds");
  return Check(bThenA == L"/virtual/tie/b/textures/foo.dds",
               "default-priority score ties follow the reverse index order");
}

bool CheckBareFilenamePriorityAndOrder() {
  hpl::cFileSearcher prioritized;
  prioritized.AddDirectory(L"/virtual/bare/stock", "*", false, 0);
  prioritized.AddDirectory(L"/virtual/bare/override", "*", false, 10);

  int equalCount = -1;
  const hpl::tWString &overrideResult =
      prioritized.GetFilePath("foo.dds", &equalCount);
  if (!Check(overrideResult == L"/virtual/bare/override/foo.dds",
             "a bare filename still prefers the higher-priority candidate") ||
      !Check(equalCount == 0,
             "a bare filename reports zero path-score matches")) {
    return false;
  }

  hpl::cFileSearcher stockFirst;
  stockFirst.AddDirectory(L"/virtual/bare/stock", "*", false);
  stockFirst.AddDirectory(L"/virtual/bare/override", "*", false);
  equalCount = -1;
  const hpl::tWString &stockResult =
      stockFirst.GetFilePath("foo.dds", &equalCount);
  if (!Check(stockResult == L"/virtual/bare/stock/foo.dds",
             "default-priority bare names keep the first indexed candidate") ||
      !Check(equalCount == 0,
             "default-priority bare names report zero path-score matches")) {
    return false;
  }

  hpl::cFileSearcher overrideFirst;
  overrideFirst.AddDirectory(L"/virtual/bare/override", "*", false);
  overrideFirst.AddDirectory(L"/virtual/bare/stock", "*", false);
  equalCount = -1;
  const hpl::tWString &reverseResult =
      overrideFirst.GetFilePath("foo.dds", &equalCount);
  return Check(reverseResult == L"/virtual/bare/override/foo.dds",
               "default-priority bare names follow the reverse index order") &&
         Check(equalCount == 0,
               "reverse-order bare names report zero path-score matches");
}

bool CheckEqualNonDefaultPriorityFallback() {
  hpl::cFileSearcher bestScore;
  bestScore.AddDirectory(L"/virtual/equal/score/loser", "*", false, 100);
  bestScore.AddDirectory(L"/virtual/equal/score/winner/materials", "*",
                         false, 100);

  int equalCount = -1;
  const hpl::tWString &resolved =
      bestScore.GetFilePath("wanted/materials/foo.dds", &equalCount);
  if (!Check(resolved == L"/virtual/equal/score/winner/materials/foo.dds",
             "equal non-default priorities fall back to path score") ||
      !Check(equalCount == 1,
             "equal non-default priorities report the winning path score")) {
    return false;
  }

  hpl::cFileSearcher firstA;
  firstA.AddDirectory(L"/virtual/tie/a/textures", "*", false, 100);
  firstA.AddDirectory(L"/virtual/tie/b/textures", "*", false, 100);
  const hpl::tWString &aThenB = firstA.GetFilePath("wanted/textures/foo.dds");
  if (!Check(aThenB == L"/virtual/tie/a/textures/foo.dds",
             "equal non-default priority ties keep the first indexed candidate")) {
    return false;
  }

  hpl::cFileSearcher firstB;
  firstB.AddDirectory(L"/virtual/tie/b/textures", "*", false, 100);
  firstB.AddDirectory(L"/virtual/tie/a/textures", "*", false, 100);
  const hpl::tWString &bThenA = firstB.GetFilePath("wanted/textures/foo.dds");
  return Check(bThenA == L"/virtual/tie/b/textures/foo.dds",
               "equal non-default priority ties follow the reverse index order");
}

bool CheckReaddingFirstDirectoryKeepsDistinctPaths() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/dup/first", "*", false);
  searcher.AddDirectory(L"/virtual/dup/second", "*", false);
  searcher.AddDirectory(L"/virtual/dup/first", "*", false);

  hpl::tWStringVec paths;
  const size_t count = searcher.GetAllFilePaths("foo.dds", paths);
  int firstCount = 0;
  int secondCount = 0;
  for (const hpl::tWString &path : paths) {
    if (path == L"/virtual/dup/first/foo.dds")
      ++firstCount;
    if (path == L"/virtual/dup/second/foo.dds")
      ++secondCount;
  }

  if (!Check(count == 2 && paths.size() == 2,
             "re-adding the first directory keeps two matching paths") ||
      !Check(firstCount == 1 && secondCount == 1,
             "re-adding the first directory keeps each matching path once")) {
    return false;
  }

  // Re-adding the LAST indexed directory is the case the old find()-based
  // guard missed: find() lands on the earliest equivalent entry, whose path
  // belongs to the other directory, so the guard never fired.
  hpl::cFileSearcher readdLast;
  readdLast.AddDirectory(L"/virtual/dup/first", "*", false);
  readdLast.AddDirectory(L"/virtual/dup/second", "*", false);
  readdLast.AddDirectory(L"/virtual/dup/second", "*", false);

  paths.clear();
  const size_t lastCount = readdLast.GetAllFilePaths("foo.dds", paths);
  firstCount = 0;
  secondCount = 0;
  for (const hpl::tWString &path : paths) {
    if (path == L"/virtual/dup/first/foo.dds")
      ++firstCount;
    if (path == L"/virtual/dup/second/foo.dds")
      ++secondCount;
  }

  return Check(lastCount == 2 && paths.size() == 2,
               "re-adding the last directory keeps two matching paths") &&
         Check(firstCount == 1 && secondCount == 1,
               "re-adding the last directory keeps each matching path once");
}

bool CheckReaddingRecursiveDirectoryKeepsCounts() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/dup/repeat", "*", true);

  hpl::tWStringVec paths;
  const size_t initialCount =
      searcher.GetAllFilePaths("repeat.dds", paths);
  searcher.AddDirectory(L"/virtual/dup/repeat", "*", true);
  searcher.AddDirectory(L"/virtual/dup/repeat", "*", true);

  paths.clear();
  const size_t repeatedCount =
      searcher.GetAllFilePaths("repeat.dds", paths);
  return Check(initialCount == 2,
               "recursive indexing finds both repeat-tree files") &&
         Check(repeatedCount == 2 && paths.size() == 2,
               "re-adding a recursive directory leaves file counts unchanged");
}

bool CheckReaddingDirectoryRescansForNewFiles() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/rescan", "*", false);
  g_fakeDirectories[L"/virtual/rescan"].files.push_back(L"added.dds");
  searcher.AddDirectory(L"/virtual/rescan", "*", false);

  hpl::tWStringVec paths;
  const size_t addedCount = searcher.GetAllFilePaths("added.dds", paths);
  if (!Check(searcher.GetFilePath("added.dds") ==
                 L"/virtual/rescan/added.dds",
             "re-adding a directory indexes a file added after the first scan") ||
      !Check(addedCount == 1 && paths.size() == 1 &&
                 paths[0] == L"/virtual/rescan/added.dds",
             "the newly indexed file has one matching entry")) {
    return false;
  }

  paths.clear();
  const size_t originalCount =
      searcher.GetAllFilePaths("original.dds", paths);
  return Check(originalCount == 1 && paths.size() == 1 &&
                   paths[0] == L"/virtual/rescan/original.dds",
               "re-scanning keeps one index entry for the original file");
}

bool CheckReaddingDirectoryKeepsHighestPriority() {
  hpl::cFileSearcher raisedAfterReadd;
  raisedAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*",
                                false, 0);
  raisedAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*",
                                false, 10);

  hpl::tWStringVec paths;
  const size_t raisedCount =
      raisedAfterReadd.GetAllFilePaths("foo.dds", paths);
  if (!Check(raisedCount == 1 && paths.size() == 1,
             "raising a re-added directory keeps one indexed entry")) {
    return false;
  }

  hpl::cFileSearcher raisedAgainstDefault;
  raisedAgainstDefault.AddDirectory(L"/virtual/dup/priority/default", "*",
                                    false);
  raisedAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                    false, 0);
  raisedAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                    false, 10);
  const hpl::tWString &raisedResult =
      raisedAgainstDefault.GetFilePath("foo.dds");
  if (!Check(raisedResult == L"/virtual/dup/priority/override/foo.dds",
             "a raised re-added priority beats the default candidate")) {
    return false;
  }

  hpl::cFileSearcher raisedAboveCompeting;
  raisedAboveCompeting.AddDirectory(L"/virtual/dup/priority/readd", "*",
                                    false, 0);
  raisedAboveCompeting.AddDirectory(L"/virtual/dup/priority/readd", "*",
                                    false, 10);
  raisedAboveCompeting.AddDirectory(L"/virtual/dup/priority/competing", "*",
                                    false, 5);
  const hpl::tWString &raisedAboveCompetingResult =
      raisedAboveCompeting.GetFilePath("foo.dds");
  if (!Check(raisedAboveCompetingResult ==
                 L"/virtual/dup/priority/readd/foo.dds",
             "a re-added higher priority beats a competing priority")) {
    return false;
  }

  hpl::cFileSearcher loweredAfterReadd;
  loweredAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*",
                                 false, 10);
  loweredAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*",
                                 false, 0);

  paths.clear();
  const size_t reverseCount =
      loweredAfterReadd.GetAllFilePaths("foo.dds", paths);
  if (!Check(reverseCount == 1 && paths.size() == 1,
             "lowering a re-added directory keeps one indexed entry")) {
    return false;
  }

  hpl::cFileSearcher reverseAgainstDefault;
  reverseAgainstDefault.AddDirectory(L"/virtual/dup/priority/default", "*",
                                     false);
  reverseAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                     false, 10);
  reverseAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                     false, 0);
  const hpl::tWString &reverseResult =
      reverseAgainstDefault.GetFilePath("foo.dds");
  return Check(reverseResult == L"/virtual/dup/priority/override/foo.dds",
               "a lower re-added priority cannot displace the highest one");
}

bool CheckReaddingDeltaDirectoriesKeepsStackingPaths() {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/dup/delta/first", "*", false);
  searcher.AddDirectory(L"/virtual/dup/delta/second", "*", false);
  searcher.AddDirectory(L"/virtual/dup/delta/first", "*", false);
  searcher.AddDirectory(L"/virtual/dup/delta/second", "*", false);

  hpl::tWStringVec paths;
  const size_t count = searcher.GetAllFilePaths("level.map_delta", paths);
  return Check(count == 2 && paths.size() == 2,
               "re-added delta directories keep both stacking paths");
}

} // namespace

namespace hpl {

void cPlatform::FindFilesInDir(tWStringList &alstStrings,
                               const tWString &asDir,
                               const tWString & /*asMask*/, bool /*abAddHidden*/) {
  const FakeDirectory *directory = FindFakeDirectory(asDir);
  if (directory != nullptr)
    alstStrings.insert(alstStrings.end(), directory->files.begin(),
                       directory->files.end());
}

void cPlatform::FindFoldersInDir(tWStringList &alstStrings,
                                 const tWString &asDir,
                                 bool /*abAddHidden*/, bool /*abAddUpFolder*/) {
  const FakeDirectory *directory = FindFakeDirectory(asDir);
  if (directory != nullptr)
    alstStrings.insert(alstStrings.end(), directory->folders.begin(),
                       directory->folders.end());
}

tWString cPlatform::GetFullFilePath(const tWString &asFilePath) {
  return Normalize(asFilePath);
}

} // namespace hpl

int main() {
  SetUpFakeTree();

  if (!CheckDepthAndAdjacentNameResolution() || !CheckTieBreakOrder() ||
      !CheckMissingAndSingleCandidate()) {
    return 1;
  }

  if (!CheckHigherPriorityWinsWhenOverrideIndexedFirst() ||
      !CheckHigherPriorityWinsWhenOverrideIndexedLast() ||
      !CheckPriorityBeatsPathScore() || !CheckDefaultPriorityRegression() ||
      !CheckBareFilenamePriorityAndOrder() ||
      !CheckEqualNonDefaultPriorityFallback() ||
      !CheckReaddingFirstDirectoryKeepsDistinctPaths() ||
      !CheckReaddingRecursiveDirectoryKeepsCounts() ||
      !CheckReaddingDirectoryRescansForNewFiles() ||
      !CheckReaddingDirectoryKeepsHighestPriority() ||
      !CheckReaddingDeltaDirectoriesKeepsStackingPaths()) {
    return 1;
  }

  std::printf("file searcher checks passed\n");
  return 0;
}
