#include "pch.h"
#include "keyvalues.h"
#include "modmanager.h"
#include "filesystem.h"
#include "hookutils.h"

#include <fstream>

// hook forward defs
typedef char (*KeyValues__LoadFromBufferType)(
	void* self, const char* resourceName, const char* pBuffer, void* pFileSystem, void* a5, void* a6, int a7);
KeyValues__LoadFromBufferType KeyValues__LoadFromBuffer;
char KeyValues__LoadFromBufferHook(
	void* self, const char* resourceName, const char* pBuffer, void* pFileSystem, void* a5, void* a6, int a7);

void InitialiseKeyValues(HMODULE baseAddress)
{
	HookEnabler hook;
	ENABLER_CREATEHOOK(
		hook, (char*)baseAddress + 0x426C30, &KeyValues__LoadFromBufferHook, reinterpret_cast<LPVOID*>(&KeyValues__LoadFromBuffer));
}

void* savedFilesystemPtr;

char KeyValues__LoadFromBufferHook(void* self, const char* resourceName, const char* pBuffer, void* pFileSystem, void* a5, void* a6, int a7)
{
	// this is just to allow playlists to get a valid pFileSystem ptr for kv building, other functions that call this particular overload of
	// LoadFromBuffer seem to get called on network stuff exclusively not exactly sure what the address wanted here is, so just taking it
	// from a function call that always happens before playlists is loaded
	if (pFileSystem != nullptr)
		savedFilesystemPtr = pFileSystem;
	if (!pFileSystem && !strcmp(resourceName, "playlists"))
		pFileSystem = savedFilesystemPtr;

	return KeyValues__LoadFromBuffer(self, resourceName, pBuffer, pFileSystem, a5, a6, a7);
}

void ModManager::TryBuildKeyValues(const char* filename)
{
	spdlog::info("Building KeyValues for file {}", filename);

	std::string normalisedPath = fs::path(filename).lexically_normal().string();
	fs::path compiledPath = GetCompiledAssetsPath() / filename;
	fs::path compiledDir = compiledPath.parent_path();
	fs::create_directories(compiledDir);

	fs::path kvPath(filename);
	std::string ogFilePath = "mod_original_";
	ogFilePath += kvPath.filename().string();

	std::string newKvs = "// AUTOGENERATED: MOD PATCH KV\n";

	int patchNum = 0;

	// copy over patch kv files, and add #bases to new file, last mods' patches should be applied first
	// note: #include should be identical but it's actually just broken, thanks respawn
	for (int64_t i = m_loadedMods.size() - 1; i > -1; i--)
	{
		if (!m_loadedMods[i].Enabled)
			continue;

		size_t fileHash = STR_HASH(normalisedPath);
		auto modKv = m_loadedMods[i].KeyValues.find(fileHash);
		if (modKv != m_loadedMods[i].KeyValues.end())
		{
			// should result in smth along the lines of #include "mod_patch_5_mp_weapon_car.txt"

			std::string patchFilePath = "mod_patch_";
			patchFilePath += std::to_string(patchNum++);
			patchFilePath += "_";
			patchFilePath += kvPath.filename().string();

			newKvs += "#base \"";
			newKvs += patchFilePath;
			newKvs += "\"\n";

			fs::remove(compiledDir / patchFilePath);

			fs::copy_file(m_loadedMods[i].ModDirectory / "keyvalues" / filename, compiledDir / patchFilePath);
		}
	}

	// add original #base last, #bases don't override preexisting keys, including the ones we've just done
	newKvs += "#base \"";
	newKvs += ogFilePath;
	newKvs += "\"\n";

	// load original file, so we can parse out the name of the root obj (e.g. WeaponData for weapons)
	std::string originalFile = ReadVPKOriginalFile(filename);

	if (!originalFile.length())
	{
		spdlog::warn("Tried to patch kv {} but no base kv was found!", ogFilePath);
		return;
	}

	char rootName[64];
	memset(rootName, 0, sizeof(rootName));
	rootName[63] = '\0';

	// iterate until we hit an ascii char that isn't in a # command or comment to get root obj name
	int i = 0;
	while (!(originalFile[i] >= 65 && originalFile[i] <= 122))
	{
		// if we hit a comment or # thing, iterate until end of line
		if (originalFile[i] == '/' || originalFile[i] == '#')
			while (originalFile[i] != '\n')
				i++;

		i++;
	}

	int j = 0;
	for (int j = 0; originalFile[i] >= 65 && originalFile[i] <= 122; j++)
		rootName[j] = originalFile[i++];

	// empty kv, all the other stuff gets #base'd
	newKvs += rootName;
	newKvs += "\n{\n}\n";

	std::ofstream originalFileWriteStream(compiledDir / ogFilePath, std::ios::binary);
	originalFileWriteStream << originalFile;
	originalFileWriteStream.close();

	std::ofstream writeStream(compiledPath, std::ios::binary);
	writeStream << newKvs;
	writeStream.close();

	ModOverrideFile overrideFile;
	overrideFile.owningMod = nullptr;
	overrideFile.path = normalisedPath;

	if (m_modFiles.find(normalisedPath) == m_modFiles.end())
		m_modFiles.insert(std::make_pair(normalisedPath, overrideFile));
	else
		m_modFiles[normalisedPath] = overrideFile;
}
