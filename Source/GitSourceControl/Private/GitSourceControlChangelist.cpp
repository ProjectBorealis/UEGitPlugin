#include "GitSourceControlChangelist.h"

#if ENGINE_MAJOR_VERSION == 5
FGitSourceControlChangelist FGitSourceControlChangelist::WorkingChangelist(TEXT("Working"), true);
FGitSourceControlChangelist FGitSourceControlChangelist::StagedChangelist(TEXT("Staged"), true);
#endif
