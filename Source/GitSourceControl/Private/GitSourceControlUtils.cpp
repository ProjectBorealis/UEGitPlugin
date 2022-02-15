// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlUtils.h"

#include "GitSourceControlCommand.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlPrivatePCH.h"
#include "GitSourceControlProvider.h"
#include "HAL/PlatformProcess.h"

#if ( ENGINE_MAJOR_VERSION == 5 )
#include "HAL/PlatformFileManager.h"
#else
#include "HAL/PlatformFilemanager.h"
#endif

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "ISourceControlModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "Logging/MessageLog.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"

#include "PackageTools.h"
#include "FileHelpers.h"
#include "Misc/MessageDialog.h"

#include "Async/Async.h"

#if PLATFORM_LINUX
#include <sys/ioctl.h>
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace GitSourceControlConstants
{
/** The maximum number of files we submit in a single Git command */
const int32 MaxFilesPerBatch = 50;
} // namespace GitSourceControlConstants

FGitScopedTempFile::FGitScopedTempFile(const FText& InText)
{
	Filename = FPaths::CreateTempFilename(*FPaths::ProjectLogDir(), TEXT("Git-Temp"), TEXT(".txt"));
	if (!FFileHelper::SaveStringToFile(InText.ToString(), *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to write to temp file: %s"), *Filename);
	}
}

FGitScopedTempFile::~FGitScopedTempFile()
{
	if (FPaths::FileExists(Filename))
	{
		if (!FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Filename))
		{
			UE_LOG(LogSourceControl, Error, TEXT("Failed to delete temp file: %s"), *Filename);
		}
	}
}

const FString& FGitScopedTempFile::GetFilename() const
{
	return Filename;
}

FDateTime FGitLockedFilesCache::LastUpdated = FDateTime::MinValue();
TMap<FString, FString> FGitLockedFilesCache::LockedFiles = TMap<FString, FString>();

namespace GitSourceControlUtils
{
// Launch the Git command line process and extract its results & errors
static bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors, const int32 ExpectedReturnCode = 0)
{
	int32 ReturnCode = 0;
	FString FullCommand;
	FString LogableCommand; // short version of the command for logging purpose

	if (!InRepositoryRoot.IsEmpty())
	{
		FString RepositoryRoot = InRepositoryRoot;

		// Detect a "migrate asset" scenario (a "git add" command is applied to files outside the current project)
		if ((InFiles.Num() > 0) && !FPaths::IsRelative(InFiles[0]) && !InFiles[0].StartsWith(InRepositoryRoot))
		{
			// in this case, find the git repository (if any) of the destination Project
			FString DestinationRepositoryRoot;
			if (FindRootDirectory(FPaths::GetPath(InFiles[0]), DestinationRepositoryRoot))
			{
				RepositoryRoot = DestinationRepositoryRoot; // if found use it for the "add" command (else not, to avoid producing one more error in logs)
			}
		}

		// Specify the working copy (the root) of the git repository (before the command itself)
		FullCommand = TEXT("-C \"");
		FullCommand += RepositoryRoot;
		FullCommand += TEXT("\" ");
	}
	// then the git command itself ("status", "log", "commit"...)
	LogableCommand += InCommand;

	// Append to the command all parameters, and then finally the files
	for (const auto& Parameter : InParameters)
	{
		LogableCommand += TEXT(" ");
		LogableCommand += Parameter;
	}
	for (const auto& File : InFiles)
	{
		LogableCommand += TEXT(" \"");
		LogableCommand += File;
		LogableCommand += TEXT("\"");
	}
	// Also, Git does not have a "--non-interactive" option, as it auto-detects when there are no connected standard input/output streams

	FullCommand += LogableCommand;

#if UE_BUILD_DEBUG
	UE_LOG(LogSourceControl, Log, TEXT("RunCommand: 'git %s'"), *LogableCommand);
#endif

	const FString& PathToGitOrEnvBinary = InPathToGitBinary;
#if PLATFORM_MAC
	// The Cocoa application does not inherit shell environment variables, so add the path expected to have git-lfs to PATH
	FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
	FString GitInstallPath = FPaths::GetPath(InPathToGitBinary);

	TArray<FString> PathArray;
	PathEnv.ParseIntoArray(PathArray, FPlatformMisc::GetPathVarDelimiter());
	bool bHasGitInstallPath = false;
	for (auto Path : PathArray)
	{
		if (GitInstallPath.Equals(Path, ESearchCase::CaseSensitive))
		{
			bHasGitInstallPath = true;
			break;
		}
	}

	if (!bHasGitInstallPath)
	{
		PathToGitOrEnvBinary = FString("/usr/bin/env");
		FullCommand = FString::Printf(TEXT("PATH=\"%s%s%s\" \"%s\" %s"), *GitInstallPath, FPlatformMisc::GetPathVarDelimiter(), *PathEnv, *InPathToGitBinary, *FullCommand);
	}
#endif

	FPlatformProcess::ExecProcess(*PathToGitOrEnvBinary, *FullCommand, &ReturnCode, &OutResults, &OutErrors);

#if UE_BUILD_DEBUG
	// TODO: add a setting to easily enable Verbose logging
	UE_LOG(LogSourceControl, Verbose, TEXT("RunCommand(%s):\n%s"), *InCommand, *OutResults);
	if (ReturnCode != ExpectedReturnCode)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("RunCommand(%s) ReturnCode=%d:\n%s"), *InCommand, ReturnCode, *OutErrors);
	}
#endif

	// Move push/pull progress information from the error stream to the info stream
	if(ReturnCode == ExpectedReturnCode && OutErrors.Len() > 0)
	{
		OutResults.Append(OutErrors);
		OutErrors.Empty();
	}

	return ReturnCode == ExpectedReturnCode;
}

// Basic parsing or results & errors from the Git command line process
static bool RunCommandInternal(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
							   const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult;
	FString Results;
	FString Errors;

	bResult = RunCommandInternalRaw(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, Results, Errors);
	Results.ParseIntoArray(OutResults, TEXT("\n"), true);
	Errors.ParseIntoArray(OutErrorMessages, TEXT("\n"), true);

	return bResult;
}

FString FindGitBinaryPath()
{
#if PLATFORM_WINDOWS
	// 1) First of all, look into standard install directories
	// NOTE using only "git" (or "git.exe") relying on the "PATH" envvar does not always work as expected, depending on the installation:
	// If the PATH is set with "git/cmd" instead of "git/bin",
	// "git.exe" launch "git/cmd/git.exe" that redirect to "git/bin/git.exe" and ExecProcess() is unable to catch its outputs streams.
	// First check the 64-bit program files directory:
	FString GitBinaryPath(TEXT("C:/Program Files/Git/bin/git.exe"));
	bool bFound = CheckGitAvailability(GitBinaryPath);
	if (!bFound)
	{
		// otherwise check the 32-bit program files directory.
		GitBinaryPath = TEXT("C:/Program Files (x86)/Git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
	}
	if (!bFound)
	{
		// else the install dir for the current user: C:\Users\UserName\AppData\Local\Programs\Git\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Programs/Git/cmd/git.exe"), *AppDataLocalPath);
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 2) Else, look for the version of Git bundled with SmartGit "Installer with JRE"
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
		if (!bFound)
		{
			// If git is not found in "git/bin/" subdirectory, try the "bin/" path that was in use before
			GitBinaryPath = TEXT("C:/Program Files (x86)/SmartGit/bin/git.exe");
			bFound = CheckGitAvailability(GitBinaryPath);
		}
	}

	// 3) Else, look for the local_git provided by SourceTree
	if (!bFound)
	{
		// C:\Users\UserName\AppData\Local\Atlassian\SourceTree\git_local\bin
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		GitBinaryPath = FString::Printf(TEXT("%s/Atlassian/SourceTree/git_local/bin/git.exe"), *AppDataLocalPath);
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the PortableGit provided by GitHub Desktop
	if (!bFound)
	{
		// The latest GitHub Desktop adds its binaries into the local appdata directory:
		// C:\Users\UserName\AppData\Local\GitHub\PortableGit_c2ba306e536fdf878271f7fe636a147ff37326ad\cmd
		const FString AppDataLocalPath = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
		const FString SearchPath = FString::Printf(TEXT("%s/GitHub/PortableGit_*"), *AppDataLocalPath);
		TArray<FString> PortableGitFolders;
		IFileManager::Get().FindFiles(PortableGitFolders, *SearchPath, false, true);
		if (PortableGitFolders.Num() > 0)
		{
			// FindFiles just returns directory names, so we need to prepend the root path to get the full path.
			GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/cmd/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last PortableGit found
			bFound = CheckGitAvailability(GitBinaryPath);
			if (!bFound)
			{
				// If Portable git is not found in "cmd/" subdirectory, try the "bin/" path that was in use before
				GitBinaryPath = FString::Printf(TEXT("%s/GitHub/%s/bin/git.exe"), *AppDataLocalPath, *(PortableGitFolders.Last())); // keep only the last
																																	// PortableGit found
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

	// 5) Else, look for the version of Git bundled with Tower
	if (!bFound)
	{
		GitBinaryPath = TEXT("C:/Program Files (x86)/fournova/Tower/vendor/Git/bin/git.exe");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

#elif PLATFORM_MAC
	// 1) First of all, look for the version of git provided by official git
	FString GitBinaryPath = TEXT("/usr/local/git/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);

	// 2) Else, look for the version of git provided by Homebrew
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 3) Else, look for the version of git provided by MacPorts
	if (!bFound)
	{
		GitBinaryPath = TEXT("/opt/local/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	// 4) Else, look for the version of git provided by Command Line Tools
	if (!bFound)
	{
		GitBinaryPath = TEXT("/usr/bin/git");
		bFound = CheckGitAvailability(GitBinaryPath);
	}

	{
		SCOPED_AUTORELEASE_POOL;
		NSWorkspace* SharedWorkspace = [NSWorkspace sharedWorkspace];

		// 5) Else, look for the version of local_git provided by SmartGit
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.syntevo.smartgit"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 6) Else, look for the version of local_git provided by SourceTree
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.torusknot.SourceTreeNotMAS"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git_local/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 7) Else, look for the version of local_git provided by GitHub Desktop
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.github.GitHubClient"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/app/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}

		// 8) Else, look for the version of local_git provided by Tower2
		if (!bFound)
		{
			NSURL* AppURL = [SharedWorkspace URLForApplicationWithBundleIdentifier:@"com.fournova.Tower2"];
			if (AppURL != nullptr)
			{
				NSBundle* Bundle = [NSBundle bundleWithURL:AppURL];
				GitBinaryPath = FString::Printf(TEXT("%s/git/bin/git"), *FString([Bundle resourcePath]));
				bFound = CheckGitAvailability(GitBinaryPath);
			}
		}
	}

#else
	FString GitBinaryPath = TEXT("/usr/bin/git");
	bool bFound = CheckGitAvailability(GitBinaryPath);
#endif

	if (bFound)
	{
		FPaths::MakePlatformFilename(GitBinaryPath);
	}
	else
	{
		// If we did not find a path to Git, set it empty
		GitBinaryPath.Empty();
	}

	return GitBinaryPath;
}

bool CheckGitAvailability(const FString& InPathToGitBinary, FGitVersion* OutVersion)
{
	FString InfoMessages;
	FString ErrorMessages;
	bool bGitAvailable = RunCommandInternalRaw(TEXT("version"), InPathToGitBinary, FString(), FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bGitAvailable)
	{
		if (!InfoMessages.StartsWith("git version"))
		{
			bGitAvailable = false;
		}
		else if (OutVersion)
		{
			ParseGitVersion(InfoMessages, OutVersion);
			FindGitCapabilities(InPathToGitBinary, OutVersion);
			FindGitLfsCapabilities(InPathToGitBinary, OutVersion);
		}
	}

	return bGitAvailable;
}

void ParseGitVersion(const FString& InVersionString, FGitVersion* OutVersion)
{
	// Parse "git version 2.31.1.vfs.0.3" into the string "2.31.1.vfs.0.3"
	const FString& TokenVersionStringPtr = InVersionString.RightChop(12);
	if (!TokenVersionStringPtr.IsEmpty())
	{
		// Parse the version into its numerical components
		TArray<FString> ParsedVersionString;
		TokenVersionStringPtr.ParseIntoArray(ParsedVersionString, TEXT("."));
		const int Num = ParsedVersionString.Num();
		if (Num >= 3)
		{
			if (ParsedVersionString[0].IsNumeric() && ParsedVersionString[1].IsNumeric() && ParsedVersionString[2].IsNumeric())
			{
				OutVersion->Major = FCString::Atoi(*ParsedVersionString[0]);
				OutVersion->Minor = FCString::Atoi(*ParsedVersionString[1]);
				OutVersion->Patch = FCString::Atoi(*ParsedVersionString[2]);
				if (Num >= 5)
				{
					// If labeled with fork
					if (!ParsedVersionString[3].IsNumeric())
					{
						OutVersion->Fork = ParsedVersionString[3];
						OutVersion->bIsFork = true;
						OutVersion->ForkMajor = FCString::Atoi(*ParsedVersionString[4]);
						if (Num >= 6)
						{
							OutVersion->ForkMinor = FCString::Atoi(*ParsedVersionString[5]);
							if (Num >= 7)
							{
								OutVersion->ForkPatch = FCString::Atoi(*ParsedVersionString[6]);
							}
						}
					}
				}
				if (OutVersion->bIsFork)
				{
					UE_LOG(LogSourceControl, Log, TEXT("Git version %d.%d.%d.%s.%d.%d.%d"), OutVersion->Major, OutVersion->Minor, OutVersion->Patch, *OutVersion->Fork, OutVersion->ForkMajor, OutVersion->ForkMinor, OutVersion->ForkPatch);
				}
				else
				{
					UE_LOG(LogSourceControl, Log, TEXT("Git version %d.%d.%d"), OutVersion->Major, OutVersion->Minor, OutVersion->Patch);
				}
			}
		}
	}
}

void FindGitCapabilities(const FString& InPathToGitBinary, FGitVersion* OutVersion)
{
	FString InfoMessages;
	FString ErrorMessages;
	RunCommandInternalRaw(TEXT("cat-file -h"), InPathToGitBinary, FString(), FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages, 129);
	if (InfoMessages.Contains("--filters"))
	{
		OutVersion->bHasCatFileWithFilters = true;
	}
}

void FindGitLfsCapabilities(const FString& InPathToGitBinary, FGitVersion* OutVersion)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	const bool bGitLfsAvailable = RunLFSCommand(TEXT("version"),FString(), FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bGitLfsAvailable)
	{
		OutVersion->bHasGitLfs = true;
		OutVersion->bHasGitLfsLocking = true;
		for (const auto& Info : InfoMessages)
		{
			UE_LOG(LogSourceControl, Log, TEXT("%s"), *Info);
		}
	}
}

// Find the root of the Git repository, looking from the provided path and upward in its parent directories.
bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot)
{
	bool bFound = false;
	FString PathToGitSubdirectory;
	OutRepositoryRoot = InPath;

	auto TrimTrailing = [](FString& Str, const TCHAR Char) {
		int32 Len = Str.Len();
		while (Len && Str[Len - 1] == Char)
		{
			Str = Str.LeftChop(1);
			Len = Str.Len();
		}
	};

	TrimTrailing(OutRepositoryRoot, '\\');
	TrimTrailing(OutRepositoryRoot, '/');

	while (!bFound && !OutRepositoryRoot.IsEmpty())
	{
		// Look for the ".git" subdirectory (or file) present at the root of every Git repository
		PathToGitSubdirectory = OutRepositoryRoot / TEXT(".git");
		bFound = IFileManager::Get().DirectoryExists(*PathToGitSubdirectory) || IFileManager::Get().FileExists(*PathToGitSubdirectory);
		if (!bFound)
		{
			int32 LastSlashIndex;
			if (OutRepositoryRoot.FindLastChar('/', LastSlashIndex))
			{
				OutRepositoryRoot = OutRepositoryRoot.Left(LastSlashIndex);
			}
			else
			{
				OutRepositoryRoot.Empty();
			}
		}
	}
	if (!bFound)
	{
		OutRepositoryRoot = InPath; // If not found, return the provided dir as best possible root.
	}
	return bFound;
}

void GetUserConfig(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutUserName, FString& OutUserEmail)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("user.name"));
	bResults = RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserName = InfoMessages[0];
	}

	Parameters.Reset(1);
	Parameters.Add(TEXT("user.email"));
	InfoMessages.Reset();
	bResults &= RunCommandInternal(TEXT("config"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutUserEmail = InfoMessages[0];
	}
}

bool GetBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
	const FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	if (!Provider.GetBranchName().IsEmpty())
	{
		OutBranchName = Provider.GetBranchName();
		return true;
	}
	
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--short"));
	Parameters.Add(TEXT("--quiet")); // no error message while in detached HEAD
	Parameters.Add(TEXT("HEAD"));
	bResults = RunCommand(TEXT("symbolic-ref"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutBranchName = InfoMessages[0];
	}
	else
	{
		Parameters.Reset(2);
		Parameters.Add(TEXT("-1"));
		Parameters.Add(TEXT("--format=\"%h\"")); // no error message while in detached HEAD
		bResults = RunCommand(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
		if (bResults && InfoMessages.Num() > 0)
		{
			OutBranchName = "HEAD detached at ";
			OutBranchName += InfoMessages[0];
		}
		else
		{
			bResults = false;
		}
	}

	return bResults;
}

bool GetRemoteBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName)
{
	const FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	if (!Provider.GetRemoteBranchName().IsEmpty())
	{
		OutBranchName = Provider.GetRemoteBranchName();
		return true;
	}

	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--abbrev-ref"));
	Parameters.Add(TEXT("--symbolic-full-name"));
	Parameters.Add(TEXT("@{u}"));
	bool bResults = RunCommand(TEXT("rev-parse"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(),
								InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutBranchName = InfoMessages[0];
	}
	if (!bResults)
	{
		static bool bRunOnce = true;
		if (bRunOnce)
		{
			UE_LOG(LogSourceControl, Warning, TEXT("Upstream branch not found for the current branch, skipping current branch for remote check. Please push a remote branch."));
			bRunOnce = false;
		}
	}
	return bResults;
}

bool GetCommitInfo(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutCommitId, FString& OutCommitSummary)
{
	bool bResults;
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("-1"));
	Parameters.Add(TEXT("--format=\"%H %s\""));
	bResults = RunCommandInternal(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutCommitId = InfoMessages[0].Left(40);
		OutCommitSummary = InfoMessages[0].RightChop(41);
	}

	return bResults;
}

bool GetRemoteUrl(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutRemoteUrl)
{
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("get-url"));
	Parameters.Add(TEXT("origin"));
	const bool bResults = RunCommandInternal(TEXT("remote"), InPathToGitBinary, InRepositoryRoot, Parameters, FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (bResults && InfoMessages.Num() > 0)
	{
		OutRemoteUrl = InfoMessages[0];
	}

	return bResults;
}

bool RunCommand(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters,
				const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult = true;

	if (InFiles.Num() > GitSourceControlConstants::MaxFilesPerBatch)
	{
		// Batch files up so we dont exceed command-line limits
		int32 FileCount = 0;
		while (FileCount < InFiles.Num())
		{
			TArray<FString> FilesInBatch;
			for (int32 FileIndex = 0; FileCount < InFiles.Num() && FileIndex < GitSourceControlConstants::MaxFilesPerBatch; FileIndex++, FileCount++)
			{
				FilesInBatch.Add(InFiles[FileCount]);
			}

			TArray<FString> BatchResults;
			TArray<FString> BatchErrors;
			bResult &= RunCommandInternal(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, FilesInBatch, BatchResults, BatchErrors);
			OutResults += BatchResults;
			OutErrorMessages += BatchErrors;
		}
	}
	else
	{
		bResult = RunCommandInternal(InCommand, InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
	}

	return bResult;
}

bool RunLFSCommand(const FString& InCommand, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles,
				   TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	FString BaseDir = IPluginManager::Get().FindPlugin("GitSourceControl")->GetBaseDir();
#if PLATFORM_WINDOWS
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs.exe"), *BaseDir);
#elif PLATFORM_MAC
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs-mac"), *BaseDir);
#elif PLATFORM_LINUX
	FString LFSLockBinary = FString::Printf(TEXT("%s/git-lfs"), *BaseDir);
#else
	checkf(false, TEXT("Unhandled platform for LFS binary!"));
#endif

	return GitSourceControlUtils::RunCommand(InCommand, LFSLockBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
}

// Run a Git "commit" command by batches
bool RunCommit(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles,
			   TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	bool bResult = true;

	if (InFiles.Num() > GitSourceControlConstants::MaxFilesPerBatch)
	{
		// Batch files up so we dont exceed command-line limits
		int32 FileCount = 0;
		{
			TArray<FString> FilesInBatch;
			for (int32 FileIndex = 0; FileIndex < GitSourceControlConstants::MaxFilesPerBatch; FileIndex++, FileCount++)
			{
				FilesInBatch.Add(InFiles[FileCount]);
			}
			// First batch is a simple "git commit" command with only the first files
			bResult &= RunCommandInternal(TEXT("commit"), InPathToGitBinary, InRepositoryRoot, InParameters, FilesInBatch, OutResults, OutErrorMessages);
		}

		TArray<FString> Parameters;
		for (const auto& Parameter : InParameters)
		{
			Parameters.Add(Parameter);
		}
		Parameters.Add(TEXT("--amend"));

		while (FileCount < InFiles.Num())
		{
			TArray<FString> FilesInBatch;
			for (int32 FileIndex = 0; FileCount < InFiles.Num() && FileIndex < GitSourceControlConstants::MaxFilesPerBatch; FileIndex++, FileCount++)
			{
				FilesInBatch.Add(InFiles[FileCount]);
			}
			// Next batches "amend" the commit with some more files
			TArray<FString> BatchResults;
			TArray<FString> BatchErrors;
			bResult &= RunCommandInternal(TEXT("commit"), InPathToGitBinary, InRepositoryRoot, Parameters, FilesInBatch, BatchResults, BatchErrors);
			OutResults += BatchResults;
			OutErrorMessages += BatchErrors;
		}
	}
	else
	{
		bResult = RunCommandInternal(TEXT("commit"), InPathToGitBinary, InRepositoryRoot, InParameters, InFiles, OutResults, OutErrorMessages);
	}

	return bResult;
}

/**
 * Parse informations on a file locked with Git LFS
 *
 * Examples output of "git lfs locks":
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset    SRombauts       ID:891
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset                    ID:891
Content\ThirdPersonBP\Blueprints\ThirdPersonCharacter.uasset    ID:891
 */
class FGitLfsLocksParser
{
public:
	FGitLfsLocksParser(const FString& InRepositoryRoot, const FString& InStatus, const bool bAbsolutePaths = true)
	{
		TArray<FString> Informations;
		InStatus.ParseIntoArray(Informations, TEXT("\t"), true);
		
		if (Informations.Num() >= 2)
		{
			Informations[0].TrimEndInline(); // Trim whitespace from the end of the filename
			Informations[1].TrimEndInline(); // Trim whitespace from the end of the username
			if (bAbsolutePaths)
				LocalFilename = FPaths::ConvertRelativePathToFull(InRepositoryRoot, Informations[0]);
			else
				LocalFilename = Informations[0];
			// Filename ID (or we expect it to be the username, but it's empty, or is the ID, we have to assume it's the current user)
			if (Informations.Num() == 2 || Informations[1].IsEmpty() || Informations[1].StartsWith(TEXT("ID:")))
			{
				LockUser = FGitSourceControlModule::Get().GetProvider().GetLockUser();
			}
			// Filename Username ID
			else
			{
				LockUser = MoveTemp(Informations[1]);
			}
		}
	}

	// Filename on disk
	FString LocalFilename;
	// Name of user who has file locked
	FString LockUser;
};

/**
 * @brief Extract the relative filename from a Git status result.
 *
 * Examples of status results:
M  Content/Textures/T_Perlin_Noise_M.uasset
R  Content/Textures/T_Perlin_Noise_M.uasset -> Content/Textures/T_Perlin_Noise_M2.uasset
?? Content/Materials/M_Basic_Wall.uasset
!! BasicCode.sln
 *
 * @param[in] InResult One line of status
 * @return Relative filename extracted from the line of status
 *
 * @see FGitStatusFileMatcher and StateFromGitStatus()
 */
static FString FilenameFromGitStatus(const FString& InResult)
{
	int32 RenameIndex;
	if (InResult.FindLastChar('>', RenameIndex))
	{
		// Extract only the second part of a rename "from -> to"
		return InResult.RightChop(RenameIndex + 2);
	}
	else
	{
		// Extract the relative filename from the Git status result (after the 2 letters status and 1 space)
		return InResult.RightChop(3);
	}
}

/** Match the relative filename of a Git status result with a provided absolute filename */
class FGitStatusFileMatcher
{
public:
	FGitStatusFileMatcher(const FString& InAbsoluteFilename) : AbsoluteFilename(InAbsoluteFilename)
	{}

	bool operator()(const FString& InResult) const
	{
		return AbsoluteFilename.Contains(FilenameFromGitStatus(InResult));
	}

private:
	const FString& AbsoluteFilename;
};

/**
 * Extract and interpret the file state from the given Git status result.
 * @see http://git-scm.com/docs/git-status
 * ' ' = unmodified
 * 'M' = modified
 * 'A' = added
 * 'D' = deleted
 * 'R' = renamed
 * 'C' = copied
 * 'U' = updated but unmerged
 * '?' = unknown/untracked
 * '!' = ignored
 */
class FGitStatusParser
{
public:
	FGitStatusParser(const FString& InResult)
	{
		TCHAR IndexState = InResult[0];
		TCHAR WCopyState = InResult[1];
		if ((IndexState == 'U' || WCopyState == 'U') || (IndexState == 'A' && WCopyState == 'A') || (IndexState == 'D' && WCopyState == 'D'))
		{
			// "Unmerged" conflict cases are generally marked with a "U",
			// but there are also the special cases of both "A"dded, or both "D"eleted
			FileState = EFileState::Unmerged;
			TreeState = ETreeState::Working;
			return;
		}

		if (IndexState == ' ')
		{
			TreeState = ETreeState::Working;
		}
		else if (WCopyState == ' ')
		{
			TreeState = ETreeState::Staged;
		}

		if (IndexState == '?' || WCopyState == '?')
		{
			TreeState = ETreeState::Untracked;
			FileState = EFileState::Unknown;
		}
		else if (IndexState == '!' || WCopyState == '!')
		{
			TreeState = ETreeState::Ignored;
			FileState = EFileState::Unknown;
		}
		else if (IndexState == 'A')
		{
			FileState = EFileState::Added;
		}
		else if (IndexState == 'D')
		{
			FileState = EFileState::Deleted;
		}
		else if (WCopyState == 'D')
		{
			FileState = EFileState::Missing;
		}
		else if (IndexState == 'M' || WCopyState == 'M')
		{
			FileState = EFileState::Modified;
		}
		else if (IndexState == 'R')
		{
			FileState = EFileState::Renamed;
		}
		else if (IndexState == 'C')
		{
			FileState = EFileState::Copied;
		}
		else
		{
			// Unmodified never yield a status
			FileState = EFileState::Unknown;
		}
	}

	EFileState::Type FileState;
	ETreeState::Type TreeState;
};

/**
 * Extract the status of a unmerged (conflict) file
 *
 * Example output of git ls-files --unmerged Content/Blueprints/BP_Test.uasset
100644 d9b33098273547b57c0af314136f35b494e16dcb 1	Content/Blueprints/BP_Test.uasset
100644 a14347dc3b589b78fb19ba62a7e3982f343718bc 2	Content/Blueprints/BP_Test.uasset
100644 f3137a7167c840847cd7bd2bf07eefbfb2d9bcd2 3	Content/Blueprints/BP_Test.uasset
 *
 * 1: The "common ancestor" of the file (the version of the file that both the current and other branch originated from).
 * 2: The version from the current branch (the master branch in this case).
 * 3: The version from the other branch (the test branch)
*/
class FGitConflictStatusParser
{
public:
	/** Parse the unmerge status: extract the base SHA1 identifier of the file */
	FGitConflictStatusParser(const TArray<FString>& InResults)
	{
		const FString& FirstResult = InResults[0]; // 1: The common ancestor of merged branches
		CommonAncestorFileId = FirstResult.Mid(7, 40);
	}

	FString CommonAncestorFileId; ///< SHA1 Id of the file (warning: not the commit Id)
};

/** Execute a command to get the details of a conflict */
static void RunGetConflictStatus(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, FGitSourceControlState& InOutFileState)
{
	TArray<FString> ErrorMessages;
	TArray<FString> Results;
	TArray<FString> Files;
	Files.Add(InFile);
	TArray<FString> Parameters;
	Parameters.Add(TEXT("--unmerged"));
	bool bResult = RunCommandInternal(TEXT("ls-files"), InPathToGitBinary, InRepositoryRoot, Parameters, Files, Results, ErrorMessages);
	if (bResult && Results.Num() == 3)
	{
		// Parse the unmerge status: extract the base revision (or the other branch?)
		FGitConflictStatusParser ConflictStatus(Results);
		InOutFileState.PendingMergeBaseFileHash = ConflictStatus.CommonAncestorFileId;
	}
}

TArray<UPackage*> UnlinkPackages(const TArray<FString>& InPackageNames)
{
	TArray<UPackage*> LoadedPackages;
	// UE4-COPY: ContentBrowserUtils::SyncPathsFromSourceControl()
	if (InPackageNames.Num() > 0)
	{
		TArray<FString> PackagesToUnlink;
		for (const auto& Filename : InPackageNames)
		{
			FString PackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
			{
				PackagesToUnlink.Add(*PackageName);
			}
		}
		// Form a list of loaded packages to reload...
		LoadedPackages.Reserve(PackagesToUnlink.Num());
		for (const FString& PackageName : PackagesToUnlink)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (Package)
			{
				LoadedPackages.Emplace(Package);

				// Detach the linkers of any loaded packages so that SCC can overwrite the files...
				if (!Package->IsFullyLoaded())
				{
					FlushAsyncLoading();
					Package->FullyLoad();
				}
				ResetLoaders(Package);
			}
		}
	}
	return LoadedPackages;
}

void ReloadPackages(TArray<UPackage*>& InPackagesToReload)
{
	// UE4-COPY: ContentBrowserUtils::SyncPathsFromSourceControl()
	// Syncing may have deleted some packages, so we need to unload those rather than re-load them...
	TArray<UPackage*> PackagesToUnload;
	InPackagesToReload.RemoveAll([&](UPackage* InPackage) -> bool {
		const FString PackageExtension = InPackage->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(InPackage->GetName(), PackageExtension);
		if (!FPaths::FileExists(PackageFilename))
		{
			PackagesToUnload.Emplace(InPackage);
			return true; // remove package
		}
		return false; // keep package
	});

	// Hot-reload the new packages...
	UPackageTools::ReloadPackages(InPackagesToReload);

	// Unload any deleted packages...
	UPackageTools::UnloadPackages(PackagesToUnload);
}

/// Convert filename relative to the repository root to absolute path (inplace)
void AbsoluteFilenames(const FString& InRepositoryRoot, TArray<FString>& InFileNames)
{
	for (auto& FileName : InFileNames)
	{
		FileName = FPaths::ConvertRelativePathToFull(InRepositoryRoot, FileName);
	}
}

/** Run a 'git ls-files' command to get all files tracked by Git recursively in a directory.
 *
 * Called in case of a "directory status" (no file listed in the command) when using the "Submit to Source Control" menu.
 */
bool ListFilesInDirectoryRecurse(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InDirectory, TArray<FString>& OutFiles)
{
	TArray<FString> ErrorMessages;
	TArray<FString> Directory;
	Directory.Add(InDirectory);
	const bool bResult = RunCommandInternal(TEXT("ls-files"), InPathToGitBinary, InRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), Directory, OutFiles, ErrorMessages);
	AbsoluteFilenames(InRepositoryRoot, OutFiles);
	return bResult;
}

/** Parse the array of strings results of a 'git status' command for a directory
 *
 *  Called in case of a "directory status" (no file listed in the command) ONLY to detect Deleted/Missing/Untracked files
 * since those files are not listed by the 'git ls-files' command.
 *
 * @see #ParseFileStatusResult() above for an example of a 'git status' results
 */
static void ParseDirectoryStatusResult(const bool InUsingLfsLocking, const TMap<FString, FString>& InResults, TMap<FString, FGitSourceControlState>& OutStates)
{
	// Iterate on each line of result of the status command
	for (const auto& Result : InResults)
	{
		FGitSourceControlState FileState(Result.Key);
		if (!InUsingLfsLocking)
		{
			FileState.State.LockState = ELockState::Unlockable;
		}
		FGitStatusParser StatusParser(Result.Value);
		if ((EFileState::Deleted == StatusParser.FileState) || (EFileState::Missing == StatusParser.FileState) || (ETreeState::Untracked == StatusParser.TreeState))
		{
			FileState.State.FileState = StatusParser.FileState;
			FileState.State.TreeState = StatusParser.TreeState;
			OutStates.Add(Result.Key, MoveTemp(FileState));
		}
	}
}

/** Parse the array of strings results of a 'git status' command for a provided list of files all in a common directory
 *
 * Called in case of a normal refresh of status on a list of assets in a the Content Browser (or user selected "Refresh" context menu).
 *
 * Example git status results:
M  Content/Textures/T_Perlin_Noise_M.uasset
R  Content/Textures/T_Perlin_Noise_M.uasset -> Content/Textures/T_Perlin_Noise_M2.uasset
?? Content/Materials/M_Basic_Wall.uasset
!! BasicCode.sln
*/
static void ParseFileStatusResult(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TSet<FString>& InFiles,
								  const TMap<FString, FString>& InResults, TMap<FString, FGitSourceControlState>& OutStates)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FString& LfsUserName = GitSourceControl.GetProvider().GetLockUser();

	TMap<FString, FString> LockedFiles;
	TMap<FString, FString> Results = InResults;
	bool bCheckedLockedFiles = false;

	FString Result;

	// Iterate on all files explicitly listed in the command
	for (const auto& File : InFiles)
	{
		FGitSourceControlState FileState(File);
		FileState.State.FileState = EFileState::Unset;
		FileState.State.TreeState = ETreeState::Unset;
		FileState.State.LockState = ELockState::Unset;
		// Search the file in the list of status
		bool bFound = Results.RemoveAndCopyValue(File, Result);
		if (bFound)
		{
			// File found in status results; only the case for "changed" files
			FGitStatusParser StatusParser(Result);
#if UE_BUILD_DEBUG
			UE_LOG(LogSourceControl, Log, TEXT("Status(%s) = '%s' => File:%d, Tree:%d"), *File, *Result, static_cast<int>(StatusParser.FileState), static_cast<int>(StatusParser.TreeState));
#endif

			FileState.State.FileState = StatusParser.FileState;
			FileState.State.TreeState = StatusParser.TreeState;
			if (FileState.IsConflicted())
			{
				// In case of a conflict (unmerged file) get the base revision to merge
				RunGetConflictStatus(InPathToGitBinary, InRepositoryRoot, File, FileState);
			}
		}
		else
		{
			FileState.State.FileState = EFileState::Unknown;
			// File not found in status
			if (FPaths::FileExists(File))
			{
				// usually means the file is unchanged,
				FileState.State.TreeState = ETreeState::Unmodified;
#if UE_BUILD_DEBUG
				UE_LOG(LogSourceControl, Log, TEXT("Status(%s) not found but exists => unchanged"), *File);
#endif
			}
			else
			{
				// but also the case for newly created content: there is no file on disk until the content is saved for the first time
				FileState.State.TreeState = ETreeState::NotInRepo;
#if UE_BUILD_DEBUG
				UE_LOG(LogSourceControl, Log, TEXT("Status(%s) not found and does not exists => new/not controled"), *File);
#endif
			}
		}
		if (!InUsingLfsLocking)
		{
			FileState.State.LockState = ELockState::Unlockable;
		}
		else
		{
			if (IsFileLFSLockable(File))
			{
				if (!bCheckedLockedFiles)
				{
					bCheckedLockedFiles = true;
					TArray<FString> ErrorMessages;
					GetAllLocks(InRepositoryRoot, ErrorMessages, LockedFiles);
					FMessageLog SourceControlLog("SourceControl");
					for (int32 ErrorIndex = 0; ErrorIndex < ErrorMessages.Num(); ++ErrorIndex)
					{
						SourceControlLog.Error(FText::FromString(ErrorMessages[ErrorIndex]));
					}
				}
				if (LockedFiles.Contains(File))
				{
					FileState.State.LockUser = LockedFiles[File];
					if (LfsUserName == FileState.State.LockUser)
					{
						FileState.State.LockState = ELockState::Locked;
					}
					else
					{
						FileState.State.LockState = ELockState::LockedOther;
					}
				}
				else
				{
					FileState.State.LockState = ELockState::NotLocked;
#if UE_BUILD_DEBUG
					UE_LOG(LogSourceControl, Log, TEXT("Status(%s) Not Locked"), *File);
#endif
				}
			}
			else
			{
				FileState.State.LockState = ELockState::Unlockable;
			}
			
			
#if UE_BUILD_DEBUG
			UE_LOG(LogSourceControl, Log, TEXT("Status(%s) Locked by '%s'"), *File, *FileState.State.LockUser);
#endif
		}
		OutStates.Add(File, MoveTemp(FileState));
	}

	// The above cannot detect deleted assets since there is no file left to enumerate (either by the Content Browser or by git ls-files)
	// => so we also parse the status results to explicitly look for Deleted/Missing assets
	ParseDirectoryStatusResult(InUsingLfsLocking, Results, OutStates);
}

/**
 * @brief Detects how to parse the result of a "status" command to get workspace file states
 *
 *  It is either a command for a whole directory (ie. "Content/", in case of "Submit to Source Control" menu),
 * or for one or more files all on a same directory (by design, since we group files by directory in RunUpdateStatus())
 *
 * @param[in]	InPathToGitBinary	The path to the Git binary
 * @param[in]	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param[in]	InUsingLfsLocking	Tells if using the Git LFS file Locking workflow
 * @param[in]	InFiles				List of files in a directory, or the path to the directory itself (never empty).
 * @param[out]	InResults			Results from the "status" command
 * @param[out]	OutStates			States of files for witch the status has been gathered (distinct than InFiles in case of a "directory status")
 */
static void ParseStatusResults(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TArray<FString>& InFiles,
							   const TMap<FString, FString>& InResults, TMap<FString, FGitSourceControlState>& OutStates)
{
	TSet<FString> Files;
	for (const auto& File : InFiles)
	{
		if (FPaths::DirectoryExists(File))
		{
			TArray<FString> DirectoryFiles;
			const bool bResult = ListFilesInDirectoryRecurse(InPathToGitBinary, InRepositoryRoot, File, DirectoryFiles);
			if (bResult)
			{
				for (const auto& InnerFile : DirectoryFiles)
				{
					Files.Add(InnerFile);
				}
			}
		}
		else
		{
			Files.Add(File);
		}
	}
	ParseFileStatusResult(InPathToGitBinary, InRepositoryRoot, InUsingLfsLocking, Files, InResults, OutStates);
}

void CheckRemote(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& Files,
				 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlState>& OutStates)
{
	// We can obtain a list of files that were modified between our remote branches and HEAD. Assumes that fetch has been run to get accurate info.

	// Gather valid remote branches
	TArray<FString> ErrorMessages;

	TSet<FString> BranchesToDiff{ FGitSourceControlModule::Get().GetProvider().GetStatusBranchNames() };

	bool bDiffAgainstRemoteCurrent = false;

	// Get the current branch's remote.
	FString CurrentBranchName;
	if (GetRemoteBranchName(InPathToGitBinary, InRepositoryRoot, CurrentBranchName))
	{
		// We have a valid remote, so diff against it.
		bDiffAgainstRemoteCurrent = true;
		// Ensure that the remote branch is in there.
		BranchesToDiff.Add(CurrentBranchName);
	}

	if (!BranchesToDiff.Num())
	{
		return;
	}

	TArray<FString> Results;
	TMap<FString, FString> NewerFiles;

	const TArray<FString>& RelativeFiles = RelativeFilenames(Files, InRepositoryRoot);
	// Get the full remote status of the Content folder, since it's the only lockable folder we track in editor. 
	// This shows any new files as well.
	// Also update the status of `.checksum`.
	TArray<FString> FilesToDiff{FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()), ".checksum"};

	TArray<FString> ParametersLog{TEXT("--pretty="), TEXT("--name-only"), TEXT(""), TEXT("--")};
	for (auto& Branch : BranchesToDiff)
	{
		bool bCurrentBranch;
		if (bDiffAgainstRemoteCurrent && Branch.Equals(CurrentBranchName))
		{
			bCurrentBranch = true;
		}
		else
		{
			bCurrentBranch = false;
		}
		// empty defaults to HEAD
		// .. means commits in the right that are not in the left
		ParametersLog[2] = FString::Printf(TEXT("..%s"), *Branch);

		const bool bResultDiff = RunCommand(TEXT("log"), InPathToGitBinary, InRepositoryRoot, ParametersLog, FilesToDiff, Results, ErrorMessages);
		if (bResultDiff)
		{
			for (const FString& NewerFileName : Results)
			{
				// Don't care about mergeable files (.collection, .ini, .uproject, etc)
				if (!IsFileLFSLockable(NewerFileName))
				{
					// Check if there's newer binaries pending on this branch
					if (bCurrentBranch && NewerFileName == TEXT(".checksum"))
					{
						FGitSourceControlModule::Get().GetProvider().bPendingRestart = true;
					}
					continue;
				}
				const FString& NewerFilePath = FPaths::ConvertRelativePathToFull(InRepositoryRoot, NewerFileName);
				if (bCurrentBranch || !NewerFiles.Contains(NewerFilePath))
				{
					NewerFiles.Add(NewerFilePath, Branch);
				}
			}
		}
		Results.Reset();
	}

	for (const auto& NewFile : NewerFiles)
	{
		if (FGitSourceControlState* FileState = OutStates.Find(NewFile.Key))
		{
			FileState->State.RemoteState = NewFile.Value.Equals(CurrentBranchName) ? ERemoteState::NotAtHead : ERemoteState::NotLatest;
			FileState->State.HeadBranch = NewFile.Value;
		}
	}

	OutErrorMessages.Append(ErrorMessages);
}

const FTimespan CacheLimit = FTimespan::FromSeconds(30);

bool GetAllLocks(const FString& InRepositoryRoot, TArray<FString>& OutErrorMessages, TMap<FString, FString>& OutLocks, bool bInvalidateCache)
{
	// You may ask, why are we ignoring state cache, and instead maintaining our own lock cache?
	// The answer is that state cache updating is another operation, and those that update status
	// (and thus the state cache) are using GetAllLocks. However, querying remote locks are almost always
	// irrelevant in most of those update status cases. So, we need to provide a fast way to provide
	// an updated local lock state. We could do this through the relevant lfs lock command arguments, which
	// as you will see below, we use only for offline cases, but the exec cost of doing this isn't worth it
	// when we can easily maintain this cache here. So, we are really emulating an internal Git LFS locks cache
	// call, which gets fed into the state cache, rather than reimplementing the state cache :)
	const FDateTime CurrentTime = FDateTime::Now();
	bool bCacheExpired = bInvalidateCache;
	if (!bInvalidateCache)
	{
		const FTimespan CacheTimeElapsed = CurrentTime - FGitLockedFilesCache::LastUpdated;
		bCacheExpired = CacheTimeElapsed > CacheLimit;
	}
	bool bResult;
	if (bCacheExpired)
	{
		// Our cache expired, or they asked us to expire cache. Query locks directly from the remote server.
		TArray<FString> ErrorMessages;
		TArray<FString> Results;
		bResult = RunLFSCommand(TEXT("locks"), InRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(),
								Results, OutErrorMessages);
		if (bResult)
		{
			for (const FString& Result : Results)
			{
				FGitLfsLocksParser LockFile(InRepositoryRoot, Result);
#if UE_BUILD_DEBUG
				UE_LOG(LogSourceControl, Log, TEXT("LockedFile(%s, %s)"), *LockFile.LocalFilename, *LockFile.LockUser);
#endif
				OutLocks.Add(MoveTemp(LockFile.LocalFilename), MoveTemp(LockFile.LockUser));
			}
			FGitLockedFilesCache::LastUpdated = CurrentTime;
			FGitLockedFilesCache::LockedFiles = OutLocks;
			return bResult;
		}
		// We tried to invalidate the UE4 cache, but we failed for some reason. Try updating lock state from LFS cache.
		// Get the last known state of remote locks
		TArray<FString> Params;
		Params.Add(TEXT("--cached"));

		const FString& LockUser = FGitSourceControlModule::Get().GetProvider().GetLockUser();

		Results.Reset();
		bResult = RunLFSCommand(TEXT("locks"), InRepositoryRoot, Params, FGitSourceControlModule::GetEmptyStringArray(), Results, OutErrorMessages);
		for (const FString& Result : Results)
		{
			FGitLfsLocksParser LockFile(InRepositoryRoot, Result);
#if UE_BUILD_DEBUG
			UE_LOG(LogSourceControl, Log, TEXT("LockedFile(%s, %s)"), *LockFile.LocalFilename, *LockFile.LockUser);
#endif
			// Only update remote locks
			if (LockFile.LockUser != LockUser)
			{
				OutLocks.Add(MoveTemp(LockFile.LocalFilename), MoveTemp(LockFile.LockUser));
			}
		}
		// Get the latest local state of our own locks
		Params.Reset(1);
		Params.Add(TEXT("--local"));

		Results.Reset();
		bResult &= RunLFSCommand(TEXT("locks"), InRepositoryRoot, Params, FGitSourceControlModule::GetEmptyStringArray(), Results, OutErrorMessages);
		for (const FString& Result : Results)
		{
			FGitLfsLocksParser LockFile(InRepositoryRoot, Result);
#if UE_BUILD_DEBUG
			UE_LOG(LogSourceControl, Log, TEXT("LockedFile(%s, %s)"), *LockFile.LocalFilename, *LockFile.LockUser);
#endif
			// Only update local locks
			if (LockFile.LockUser == LockUser)
			{
				OutLocks.Add(MoveTemp(LockFile.LocalFilename), MoveTemp(LockFile.LockUser));
			}
		}
	}
	else
	{
		// We can use our internally tracked local lock cache (an effective combination of --cached and --local)
		OutLocks = FGitLockedFilesCache::LockedFiles;
		bResult = true;
	}
	return bResult;
}

void GetLockedFiles(const TArray<FString>& InFiles, TArray<FString>& OutFiles)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(InFiles, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		const auto& GitState = StaticCastSharedRef<FGitSourceControlState>(State);
		if (GitState->State.LockState == ELockState::Locked)
		{
			OutFiles.Add(GitState->GetFilename());
		}
	}
}

// Run a batch of Git "status" command to update status of given files and/or directories.
bool RunUpdateStatus(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const bool InUsingLfsLocking, const TArray<FString>& InFiles,
					 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlState>& OutStates)
{
	// Remove files that aren't in the repository
	const TArray<FString>& RepoFiles = InFiles.FilterByPredicate([InRepositoryRoot](const FString& File) { return File.StartsWith(InRepositoryRoot); });

	if (!RepoFiles.Num())
	{
		return false;
	}

	TArray<FString> Parameters;
	Parameters.Add(TEXT("--porcelain"));
	Parameters.Add(TEXT("-unormal")); // make sure we use -unormal (user can customize it)
	// We skip checking ignored since no one ignores files that Unreal would read in as source controlled (Content/{*.uasset,*.umap},Config/*.ini).
	TArray<FString> Results;
	// avoid locking the index when not needed (useful for status updates)
	const bool bResult = RunCommand(TEXT("--no-optional-locks status"), InPathToGitBinary, InRepositoryRoot, Parameters, RepoFiles, Results, OutErrorMessages);
	TMap<FString, FString> ResultsMap;
	for (const auto& Result : Results)
	{
		const FString& RelativeFilename = FilenameFromGitStatus(Result);
		const FString& File = FPaths::ConvertRelativePathToFull(InRepositoryRoot, RelativeFilename);
		ResultsMap.Add(File, Result);
	}
	if (bResult)
	{
		ParseStatusResults(InPathToGitBinary, InRepositoryRoot, InUsingLfsLocking, RepoFiles, ResultsMap, OutStates);
	}

	CheckRemote(InPathToGitBinary, InRepositoryRoot, RepoFiles, OutErrorMessages, OutStates);

	return bResult;
}

// Run a Git `cat-file --filters` command to dump the binary content of a revision into a file.
bool RunDumpToFile(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InParameter, const FString& InDumpFileName)
{
	int32 ReturnCode = -1;
	FString FullCommand;

	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FGitVersion& GitVersion = GitSourceControl.GetProvider().GetGitVersion();

	if (!InRepositoryRoot.IsEmpty())
	{
		// Specify the working copy (the root) of the git repository (before the command itself)
		FullCommand = TEXT("-C \"");
		FullCommand += InRepositoryRoot;
		FullCommand += TEXT("\" ");
	}

	// then the git command itself
	if (GitVersion.bHasCatFileWithFilters)
	{
		// Newer versions (2.9.3.windows.2) support smudge/clean filters used by Git LFS, git-fat, git-annex, etc
		FullCommand += TEXT("cat-file --filters ");
	}
	else
	{
		// Previous versions fall-back on "git show" like before
		FullCommand += TEXT("show ");
	}

	// Append to the command the parameter
	FullCommand += InParameter;

	const bool bLaunchDetached = false;
	const bool bLaunchHidden = true;
	const bool bLaunchReallyHidden = bLaunchHidden;

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;

	verify(FPlatformProcess::CreatePipe(PipeRead, PipeWrite));

	UE_LOG(LogSourceControl, Log, TEXT("RunDumpToFile: 'git %s'"), *FullCommand);

    FString PathToGitOrEnvBinary = InPathToGitBinary;
    #if PLATFORM_MAC
        // The Cocoa application does not inherit shell environment variables, so add the path expected to have git-lfs to PATH
        FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
        FString GitInstallPath = FPaths::GetPath(InPathToGitBinary);

        TArray<FString> PathArray;
        PathEnv.ParseIntoArray(PathArray, FPlatformMisc::GetPathVarDelimiter());
        bool bHasGitInstallPath = false;
        for (auto Path : PathArray)
        {
            if (GitInstallPath.Equals(Path, ESearchCase::CaseSensitive))
            {
                bHasGitInstallPath = true;
                break;
            }
        }

        if (!bHasGitInstallPath)
        {
            PathToGitOrEnvBinary = FString("/usr/bin/env");
            FullCommand = FString::Printf(TEXT("PATH=\"%s%s%s\" \"%s\" %s"), *GitInstallPath, FPlatformMisc::GetPathVarDelimiter(), *PathEnv, *InPathToGitBinary, *FullCommand);
        }
    #endif
    
	FProcHandle ProcessHandle = FPlatformProcess::CreateProc(*PathToGitOrEnvBinary, *FullCommand, bLaunchDetached, bLaunchHidden, bLaunchReallyHidden, nullptr, 0, *InRepositoryRoot, PipeWrite);
	if(ProcessHandle.IsValid())
	{
		FPlatformProcess::Sleep(0.01f);

		TArray<uint8> BinaryFileContent;
		bool bRemovedLFSMessage = false;
		while (FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			TArray<uint8> BinaryData;
			FPlatformProcess::ReadPipeToArray(PipeRead, BinaryData);
			if (BinaryData.Num() > 0)
			{
				// @todo: this is hacky!
				if (BinaryData[0] == 68) // Check for D in "Downloading"
				{
					if (BinaryData[BinaryData.Num() - 1] == 10) // Check for newline
					{
						BinaryData.Reset();
						bRemovedLFSMessage = true;
					}
				}
				else
				{
					BinaryFileContent.Append(MoveTemp(BinaryData));
				}
			}
		}
		TArray<uint8> BinaryData;
		FPlatformProcess::ReadPipeToArray(PipeRead, BinaryData);
		if (BinaryData.Num() > 0)
		{
			// @todo: this is hacky!
			if (!bRemovedLFSMessage && BinaryData[0] == 68) // Check for D in "Downloading"
			{
				int32 NewLineIndex = 0;
				for (int32 Index = 0; Index < BinaryData.Num(); Index++)
				{
					if (BinaryData[Index] == 10) // Check for newline
					{
						NewLineIndex = Index;
						break;
					}
				}
				if (NewLineIndex > 0)
				{
					BinaryData.RemoveAt(0, NewLineIndex + 1);
				}
			}
			else
			{
				BinaryFileContent.Append(MoveTemp(BinaryData));
			}
		}

		FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
		if (ReturnCode == 0)
		{
			// Save buffer into temp file
			if (FFileHelper::SaveArrayToFile(BinaryFileContent, *InDumpFileName))
			{
				UE_LOG(LogSourceControl, Log, TEXT("Wrote '%s' (%do)"), *InDumpFileName, BinaryFileContent.Num());
			}
			else
			{
				UE_LOG(LogSourceControl, Error, TEXT("Could not write %s"), *InDumpFileName);
				ReturnCode = -1;
			}
		}
		else
		{
			UE_LOG(LogSourceControl, Error, TEXT("DumpToFile: ReturnCode=%d"), ReturnCode);
		}

		FPlatformProcess::CloseProc(ProcessHandle);
	}
	else
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to launch 'git cat-file'"));
	}

	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	return (ReturnCode == 0);
}

/**
 * Translate file actions from the given Git log --name-status command to keywords used by the Editor UI.
 *
 * @see https://www.kernel.org/pub/software/scm/git/docs/git-log.html
 * ' ' = unmodified
 * 'M' = modified
 * 'A' = added
 * 'D' = deleted
 * 'R' = renamed
 * 'C' = copied
 * 'T' = type changed
 * 'U' = updated but unmerged
 * 'X' = unknown
 * 'B' = broken pairing
 *
 * @see SHistoryRevisionListRowContent::GenerateWidgetForColumn(): "add", "edit", "delete", "branch" and "integrate" (everything else is taken like "edit")
 */
static FString LogStatusToString(TCHAR InStatus)
{
	switch (InStatus)
	{
		case TEXT(' '):
			return FString("unmodified");
		case TEXT('M'):
			return FString("modified");
		case TEXT('A'): // added: keyword "add" to display a specific icon instead of the default "edit" action one
			return FString("add");
		case TEXT('D'): // deleted: keyword "delete" to display a specific icon instead of the default "edit" action one
			return FString("delete");
		case TEXT('R'): // renamed keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('C'): // copied keyword "branch" to display a specific icon instead of the default "edit" action one
			return FString("branch");
		case TEXT('T'):
			return FString("type changed");
		case TEXT('U'):
			return FString("unmerged");
		case TEXT('X'):
			return FString("unknown");
		case TEXT('B'):
			return FString("broked pairing");
	}

	return FString();
}

/**
 * Parse the array of strings results of a 'git log' command
 *
 * Example git log results:
commit 97a4e7626681895e073aaefd68b8ac087db81b0b
Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
Date:   2014-2015-05-15 21:32:27 +0200

	Another commit used to test History

	 - with many lines
	 - some <xml>
	 - and strange characteres $*+

M	Content/Blueprints/Blueprint_CeilingLight.uasset
R100	Content/Textures/T_Concrete_Poured_D.uasset Content/Textures/T_Concrete_Poured_D2.uasset

commit 355f0df26ebd3888adbb558fd42bb8bd3e565000
Author: Sébastien Rombauts <sebastien.rombauts@gmail.com>
Date:   2014-2015-05-12 11:28:14 +0200

	Testing git status, edit, and revert

A	Content/Blueprints/Blueprint_CeilingLight.uasset
C099	Content/Textures/T_Concrete_Poured_N.uasset Content/Textures/T_Concrete_Poured_N2.uasset
*/
static void ParseLogResults(const TArray<FString>& InResults, TGitSourceControlHistory& OutHistory)
{
	TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe> SourceControlRevision = MakeShareable(new FGitSourceControlRevision);
	for (const auto& Result : InResults)
	{
		if (Result.StartsWith(TEXT("commit "))) // Start of a new commit
		{
			// End of the previous commit
			if (SourceControlRevision->RevisionNumber != 0)
			{
				OutHistory.Add(MoveTemp(SourceControlRevision));

				SourceControlRevision = MakeShareable(new FGitSourceControlRevision);
			}
			SourceControlRevision->CommitId = Result.RightChop(7); // Full commit SHA1 hexadecimal string
			SourceControlRevision->ShortCommitId = SourceControlRevision->CommitId.Left(8); // Short revision ; first 8 hex characters (max that can hold a 32
																							// bit integer)
			SourceControlRevision->CommitIdNumber = FParse::HexNumber(*SourceControlRevision->ShortCommitId);
			SourceControlRevision->RevisionNumber = -1; // RevisionNumber will be set at the end, based off the index in the History
		}
		else if (Result.StartsWith(TEXT("Author: "))) // Author name & email
		{
			// Remove the 'email' part of the UserName
			FString UserNameEmail = Result.RightChop(8);
			int32 EmailIndex = 0;
			if (UserNameEmail.FindLastChar('<', EmailIndex))
			{
				SourceControlRevision->UserName = UserNameEmail.Left(EmailIndex - 1);
			}
		}
		else if (Result.StartsWith(TEXT("Date:   "))) // Commit date
		{
			FString Date = Result.RightChop(8);
			SourceControlRevision->Date = FDateTime::FromUnixTimestamp(FCString::Atoi(*Date));
		}
		//	else if(Result.IsEmpty()) // empty line before/after commit message has already been taken care by FString::ParseIntoArray()
		else if (Result.StartsWith(TEXT("    "))) // Multi-lines commit message
		{
			SourceControlRevision->Description += Result.RightChop(4);
			SourceControlRevision->Description += TEXT("\n");
		}
		else // Name of the file, starting with an uppercase status letter ("A"/"M"...)
		{
			const TCHAR Status = Result[0];
			SourceControlRevision->Action = LogStatusToString(Status); // Readable action string ("Added", Modified"...) instead of "A"/"M"...
			// Take care of special case for Renamed/Copied file: extract the second filename after second tabulation
			int32 IdxTab;
			if (Result.FindLastChar('\t', IdxTab))
			{
				SourceControlRevision->Filename = Result.RightChop(IdxTab + 1); // relative filename
			}
		}
	}
	// End of the last commit
	if (SourceControlRevision->RevisionNumber != 0)
	{
		OutHistory.Add(MoveTemp(SourceControlRevision));
	}

	// Then set the revision number of each Revision based on its index (reverse order since the log starts with the most recent change)
	for (int32 RevisionIndex = 0; RevisionIndex < OutHistory.Num(); RevisionIndex++)
	{
		const auto& SourceControlRevisionItem = OutHistory[RevisionIndex];
		SourceControlRevisionItem->RevisionNumber = OutHistory.Num() - RevisionIndex;

		// Special case of a move ("branch" in Perforce term): point to the previous change (so the next one in the order of the log)
		if ((SourceControlRevisionItem->Action == "branch") && (RevisionIndex < OutHistory.Num() - 1))
		{
			SourceControlRevisionItem->BranchSource = OutHistory[RevisionIndex + 1];
		}
	}
}

/**
 * Extract the SHA1 identifier and size of a blob (file) from a Git "ls-tree" command.
 *
 * Example output for the command git ls-tree --long 7fdaeb2 Content/Blueprints/BP_Test.uasset
100644 blob a14347dc3b589b78fb19ba62a7e3982f343718bc   70731	Content/Blueprints/BP_Test.uasset
*/
class FGitLsTreeParser
{
public:
	/** Parse the unmerge status: extract the base SHA1 identifier of the file */
	FGitLsTreeParser(const TArray<FString>& InResults)
	{
		const FString& FirstResult = InResults[0];
		FileHash = FirstResult.Mid(12, 40);
		int32 IdxTab;
		if (FirstResult.FindChar('\t', IdxTab))
		{
			const FString SizeString = FirstResult.Mid(53, IdxTab - 53);
			FileSize = FCString::Atoi(*SizeString);
		}
	}

	FString FileHash; ///< SHA1 Id of the file (warning: not the commit Id)
	int32 FileSize; ///< Size of the file (in bytes)
};

// Run a Git "log" command and parse it.
bool RunGetHistory(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, bool bMergeConflict,
				   TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory)
{
	bool bResults;
	{
		TArray<FString> Results;
		TArray<FString> Parameters;
		Parameters.Add(TEXT("--follow")); // follow file renames
		Parameters.Add(TEXT("--date=raw"));
		Parameters.Add(TEXT("--name-status")); // relative filename at this revision, preceded by a status character
		Parameters.Add(TEXT("--pretty=medium")); // make sure format matches expected in ParseLogResults
		if (bMergeConflict)
		{
			// In case of a merge conflict, we also need to get the tip of the "remote branch" (MERGE_HEAD) before the log of the "current branch" (HEAD)
			// @todo does not work for a cherry-pick! Test for a rebase.
			Parameters.Add(TEXT("MERGE_HEAD"));
			Parameters.Add(TEXT("--max-count 1"));
		}
		else
		{
			Parameters.Add(TEXT("--max-count 250")); // Increase default count to 250 from 100
		}
		TArray<FString> Files;
		Files.Add(*InFile);
		bResults = RunCommand(TEXT("log"), InPathToGitBinary, InRepositoryRoot, Parameters, Files, Results, OutErrorMessages);
		if (bResults)
		{
			ParseLogResults(Results, OutHistory);
		}
	}
	for (auto& Revision : OutHistory)
	{
		// Get file (blob) sha1 id and size
		TArray<FString> Results;
		TArray<FString> Parameters;
		Parameters.Add(TEXT("--long")); // Show object size of blob (file) entries.
		Parameters.Add(Revision->GetRevision());
		TArray<FString> Files;
		Files.Add(*Revision->GetFilename());
		bResults &= RunCommand(TEXT("ls-tree"), InPathToGitBinary, InRepositoryRoot, Parameters, Files, Results, OutErrorMessages);
		if (bResults && Results.Num())
		{
			FGitLsTreeParser LsTree(Results);
			Revision->FileHash = LsTree.FileHash;
			Revision->FileSize = LsTree.FileSize;
		}
	}

	return bResults;
}

TArray<FString> RelativeFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> RelativeFiles;
	FString RelativeTo = InRelativeTo;

	// Ensure that the path ends w/ '/'
	if ((RelativeTo.Len() > 0) && (RelativeTo.EndsWith(TEXT("/"), ESearchCase::CaseSensitive) == false) &&
		(RelativeTo.EndsWith(TEXT("\\"), ESearchCase::CaseSensitive) == false))
	{
		RelativeTo += TEXT("/");
	}
	for (FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		if (FPaths::MakePathRelativeTo(FileName, *RelativeTo))
		{
			RelativeFiles.Add(FileName);
		}
	}

	return RelativeFiles;
}

TArray<FString> AbsoluteFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo)
{
	TArray<FString> AbsFiles;

	for(FString FileName : InFileNames) // string copy to be able to convert it inplace
	{
		AbsFiles.Add(FPaths::Combine(InRelativeTo, FileName));
	}

	return AbsFiles;
}

bool UpdateCachedStates(const TMap<const FString, FGitState>& InResults)
{
	if (InResults.Num() == 0)
	{
		return false;
	}

	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const bool bUsingGitLfsLocking = Provider.UsesCheckout();

	// TODO without LFS : Workaround a bug with the Source Control Module not updating file state after a simple "Save" with no "Checkout" (when not using File Lock)
	const FDateTime Now = bUsingGitLfsLocking ? FDateTime::Now() : FDateTime::MinValue();

	for (const auto& Pair : InResults)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(Pair.Key);
		const FGitState& NewState = Pair.Value;
		if (NewState.FileState != EFileState::Unset)
		{
			// Invalid transition
			if (NewState.FileState == EFileState::Added && !State->IsUnknown() && !State->CanAdd())
			{
				continue;
			}
			State->State.FileState = NewState.FileState;
		}
		if (NewState.TreeState != ETreeState::Unset)
		{
			State->State.TreeState = NewState.TreeState;
		}
		// If we're updating lock state, also update user
		if (NewState.LockState != ELockState::Unset)
		{
			State->State.LockState = NewState.LockState;
			State->State.LockUser = NewState.LockUser;
		}
		if (NewState.RemoteState != ERemoteState::Unset)
		{
			State->State.RemoteState = NewState.RemoteState;
			if (NewState.RemoteState == ERemoteState::UpToDate)
			{
				State->State.HeadBranch = TEXT("");
			}
			else
			{
				State->State.HeadBranch = NewState.HeadBranch;
			}
		}
		State->TimeStamp = Now;

		// We've just updated the state, no need for UpdateStatus to be ran for this file again.
		Provider.AddFileToIgnoreForceCache(State->LocalFilename);
	}

	return true;
}

bool CollectNewStates(const TMap<FString, FGitSourceControlState>& InStates, TMap<const FString, FGitState>& OutResults)
{
	if (InStates.Num() == 0)
	{
		return false;
	}
	
	for (const auto& InState : InStates)
	{
		OutResults.Add(InState.Key, InState.Value.State);
	}

	return true;
}

bool CollectNewStates(const TArray<FString>& InFiles, TMap<const FString, FGitState>& OutResults, EFileState::Type FileState, ETreeState::Type TreeState, ELockState::Type LockState, ERemoteState::Type RemoteState)
{
	if (InFiles.Num() == 0)
	{
		return false;
	}

	FGitState NewState;
	NewState.FileState = FileState;
	NewState.TreeState = TreeState;
	NewState.LockState = LockState;
	NewState.RemoteState = RemoteState;

	for (const auto& File : InFiles)
	{
		FGitState& State = OutResults.FindOrAdd(File, NewState);
		if (NewState.FileState != EFileState::Unset)
		{
			State.FileState = NewState.FileState;
		}
		if (NewState.TreeState != ETreeState::Unset)
		{
			State.TreeState = NewState.TreeState;
		}
		if (NewState.LockState != ELockState::Unset)
		{
			State.LockState = NewState.LockState;
		}
		if (NewState.RemoteState != ERemoteState::Unset)
		{
			State.RemoteState = NewState.RemoteState;
		}
	}

	return true;
}

/**
 * Helper struct for RemoveRedundantErrors()
 */
struct FRemoveRedundantErrors
{
	FRemoveRedundantErrors(const FString& InFilter) : Filter(InFilter)
	{}

	bool operator()(const FString& String) const
	{
		if (String.Contains(Filter))
		{
			return true;
		}

		return false;
	}

	/** The filter string we try to identify in the reported error */
	FString Filter;
};

void RemoveRedundantErrors(FGitSourceControlCommand& InCommand, const FString& InFilter)
{
	bool bFoundRedundantError = false;
	for (auto Iter(InCommand.ResultInfo.ErrorMessages.CreateConstIterator()); Iter; Iter++)
	{
		if (Iter->Contains(InFilter))
		{
			InCommand.ResultInfo.InfoMessages.Add(*Iter);
			bFoundRedundantError = true;
		}
	}

	InCommand.ResultInfo.ErrorMessages.RemoveAll(FRemoveRedundantErrors(InFilter));

	// if we have no error messages now, assume success!
	if (bFoundRedundantError && InCommand.ResultInfo.ErrorMessages.Num() == 0 && !InCommand.bCommandSuccessful)
	{
		InCommand.bCommandSuccessful = true;
	}
}

static TArray<FString> LockableTypes;

bool IsFileLFSLockable(const FString& InFile)
{
	for (const auto& Type : LockableTypes)
	{
		if (InFile.EndsWith(Type))
		{
			return true;
		}
	}
	return false;
}

bool CheckLFSLockable(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages)
{
	TArray<FString> Results;
	TArray<FString> Parameters;
	Parameters.Add(TEXT("lockable")); // follow file renames

	const bool bResults = RunCommand(TEXT("check-attr"), InPathToGitBinary, InRepositoryRoot, Parameters, InFiles, Results, OutErrorMessages);
	if (!bResults)
	{
		return false;
	}

	for (int i = 0; i < InFiles.Num(); i++)
	{
		const FString& Result = Results[i];
		if (Result.EndsWith("set"))
		{
			const FString FileExt = InFiles[i].RightChop(1); // Remove wildcard (*)
			LockableTypes.Add(FileExt);
		}
	}

	return true;
}

bool FetchRemote(const FString& InPathToGitBinary, const FString& InPathToRepositoryRoot, bool InUsingGitLfsLocking, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	// Force refresh lock states
	if (InUsingGitLfsLocking)
	{
		TMap<FString, FString> Locks;
		GetAllLocks(InPathToRepositoryRoot, OutErrorMessages, Locks, true);
	}
	// fetch latest repo
	// TODO specify branches?
	return RunCommand(TEXT("fetch"), InPathToGitBinary, InPathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(),
					  FGitSourceControlModule::GetEmptyStringArray(), OutResults, OutErrorMessages);
}

bool PullOrigin(const FString& InPathToGitBinary, const FString& InPathToRepositoryRoot, const TArray<FString>& InFiles, TArray<FString>& OutFiles,
				TArray<FString>& OutResults, TArray<FString>& OutErrorMessages)
{
	if (FGitSourceControlModule::Get().GetProvider().bPendingRestart)
	{
		FText PullFailMessage(LOCTEXT("Git_NeedBinariesUpdate_Msg", "Refused to Git Pull because your editor binaries are out of date.\n\n"
																	"Without a binaries update, new assets can become corrupted or cause crashes due to format "
																	"differences.\n\n"
																	"Please exit the editor, and update the project."));
		FText PullFailTitle(LOCTEXT("Git_NeedBinariesUpdate_Title", "Binaries Update Required"));
		FMessageDialog::Open(EAppMsgType::Ok, PullFailMessage, &PullFailTitle);
		UE_LOG(LogSourceControl, Log, TEXT("Pull failed because we need a binaries update"));
		return false;
	}

	const TSet<FString> AlreadyReloaded {InFiles};

	// Get remote branch
	FString RemoteBranch;
	if (!GetRemoteBranchName(InPathToGitBinary, InPathToRepositoryRoot, RemoteBranch))
	{
		// No remote to sync from
		return false;
	}

	// Get the list of files which will be updated (either ones we changed locally, which will get potentially rebased or merged, or the remote ones that will update)
	TArray<FString> DifferentFiles;
	const bool bResultDiff = RunCommand(TEXT("diff"), InPathToGitBinary, InPathToRepositoryRoot, { TEXT("--name-only"), RemoteBranch }, FGitSourceControlModule::GetEmptyStringArray(), DifferentFiles, OutErrorMessages);
	if (!bResultDiff)
	{
		return false;
	}

	// Nothing to pull
	if (!DifferentFiles.Num())
	{
		return true;
	}

	const TArray<FString>& AbsoluteDifferentFiles = AbsoluteFilenames(DifferentFiles, InPathToRepositoryRoot);

	if (AlreadyReloaded.Num())
	{
		OutFiles.Reserve(AbsoluteDifferentFiles.Num() - AlreadyReloaded.Num());
		for (const auto& File : AbsoluteDifferentFiles)
		{
			if (!AlreadyReloaded.Contains(File))
			{
				OutFiles.Add(File);
			}
		}
	}
	else
	{
		OutFiles.Append(AbsoluteDifferentFiles);
	}

	TArray<FString> Files;
	for (const auto& File : OutFiles)
	{
		if (IsFileLFSLockable(File))
		{
			Files.Add(File);
		}
	}

	const bool bShouldReload = Files.Num() > 0;
	TArray<UPackage*> PackagesToReload;
	if (bShouldReload)
	{
		const auto PackagesToReloadResult = Async(EAsyncExecution::TaskGraphMainThread, [=] {
			return UnlinkPackages(Files);
		});
		PackagesToReload = PackagesToReloadResult.Get();
	}

	// Reset HEAD and index to remote
	TArray<FString> InfoMessages;
	bool bSuccess = RunCommand(TEXT("pull"), InPathToGitBinary, InPathToRepositoryRoot, { "--rebase", "--autostash" }, FGitSourceControlModule::GetEmptyStringArray(),
										  InfoMessages, OutErrorMessages);

	if (bShouldReload)
	{
		const auto ReloadPackagesResult = Async(EAsyncExecution::TaskGraphMainThread, [=] {
			TArray<UPackage*> Packages = PackagesToReload;
			ReloadPackages(Packages);
		});
		ReloadPackagesResult.Wait();
	}

	return bSuccess;
}

} // namespace GitSourceControlUtils

#undef LOCTEXT_NAMESPACE
