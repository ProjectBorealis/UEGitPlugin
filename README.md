# Unreal Engine Git Plugin

This is a refactor of the [Git LFS 2 plugin by SRombauts](https://github.com/SRombauts/UE4GitPlugin), with lessons learned from production, performance optimizations, 
new features and workflow improvements.

## Features

* Multi-threaded locking/unlocking, greatly improving performance when locking/unlocking many files
* Remote lock status only checked when needed
* Added local lock cache for speeding up local operations
* Improved performance of repository file traversal
* Improved initialization logic
* Generally improved wording and UX throughout
* Greatly improved pull within editor
  * Only refreshes changed files, which prevents crashes in large projects
  * Uses rebase workflow to properly manage local work
* Added support for Status Branches, which check outdated files vs. remote across multiple branches
* Periodic background remote refresh to keep remote file statuses up to date
* Automatic handling of pushing from an outdated local copy
* Optimized status updates for successful operations
* Manage both lockable (assets, maps) and non-lockable files (configs, project file) in editor
* Improved status display in editor
* Integration with [PBSync](https://github.com/ProjectBorealis/PBSync) binaries syncing
* General improvements to performance and memory usage

## Status Branches

Epic Games added Status Branches in 4.20, and this plugin has implemented support for them. See [Workflow on Fortnite](https://youtu.be/p4RcDpGQ_tI?t=1443) for more information. Here is an example of how we use them in Project Borealis, and how you may apply it to your own game.

1. Make an `UEditorEngine` subclass, preferrably in an editor only module, or guarded by `WITH_EDITOR`.
2. Add the following:

```cpp
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"

void UPBEditorEngine::Init(IEngineLoop* InEngineLoop)
{
	Super::Init(InEngineLoop);

	// Register state branches
	const ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
	if (SourceControlModule.IsEnabled())
	{
		ISourceControlProvider& SourceControlProvider = SourceControlModule.GetProvider();
		// Order matters. Lower values are lower in the hierarchy, i.e., changes from higher branches get automatically merged down.
		// The higher branch is, the stabler it is, and has changes manually promoted up.
		const TArray<FString> Branches {"trunk", "main", "promoted"};
		SourceControlProvider.RegisterStateBranches(Branches, TEXT(""));
	}
}
```

3. Set to use the editor engine in `Config/DefaultEngine.ini` (make sure `ClassName` is `MyEditorEngine` for a class called `UMyEditorEngine`!):

```ini
[/Script/Engine.Engine]
UnrealEdEngine=/Script/Module.ClassName
```

5. In this example, `promoted` is the highest tested branch. Any changes in this branch are asset changes that do not need testing, and get automatically merged down to `main` and then to `trunk`. `trunk` is where cutting edge development by programmers happen, and they move up to `main`, and then `promoted` after a manual review and merge process. The second argument in `RegisterStateBranches` is Perforce specific and is ignored.
