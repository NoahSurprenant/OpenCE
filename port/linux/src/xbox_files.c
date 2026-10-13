/*
XBOX_FILES.C

Win32 file API for the Linux build, over POSIX descriptors.

Xbox paths are translated below two roots. d:\ is the data root, the
directory holding the game's maps/ folder: paths.data (port_config.c), else the current
directory when it has maps/, else assets/ in the current directory or two
levels above the executable (the repository root for build/linux/halo).
h:\ is the Halo Custom Edition install paths.custom_edition names, if any
(its maps folder holds the resource maps Custom Edition maps need, and more
maps).
Every other drive letter X:\ is the subdirectory X/ of the save root (z:\ holds the persistent cache and saves,
u:\ user data, t:\ title data): paths.saves, else
$XDG_DATA_HOME/halo-linux or ~/.local/share/halo-linux. Path components are
matched case-insensitively, like the Xbox's FATX volumes.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- paths */

static BOOL directory_exists(const char *path)
{
	struct posix_file_information information;

	return posix_stat(path, &information) == 0;
}

/* whether directory has a maps folder, in any case */
static BOOL has_maps(const char *directory)
{
	char on_disk[256];

	return directory_exists(directory) &&
		posix_find_entry_case_insensitive(directory, "maps", on_disk, sizeof(on_disk));
}

static void trim_separators(char *path)
{
	while (strlen(path) > 1 && path[strlen(path) - 1] == '/')
		path[strlen(path) - 1] = '\0';
}

const char *platform_data_root(void)
{
	static char root[MAX_PATH];

	if (!root[0])
	{
		const char *environment = config_string("paths.data");

		if (*environment)
		{
			snprintf(root, sizeof(root), "%s", environment);
		}
		else if (has_maps("."))
		{
			snprintf(root, sizeof(root), ".");
		}
		else
		{
			char executable[MAX_PATH];
			char executable_directory[MAX_PATH] = "";
			ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);

			if (length > 0)
			{
				char *slash;

				executable[length] = '\0';
				snprintf(executable_directory, sizeof(executable_directory), "%s", executable);
				slash = strrchr(executable_directory, '/');
				if (slash)
					*slash = '\0';
			}
			snprintf(root, sizeof(root), ".");
			if (executable_directory[0] && has_maps(executable_directory))
			{
				/* next to the executable, where the desktop ports' first start
				extracts it (platform_offer_game_data) */
				snprintf(root, sizeof(root), "%s", executable_directory);
			}
			else if (has_maps("assets"))
			{
				snprintf(root, sizeof(root), "assets");
			}
			else if (length > 0)
			{
				/* <repository>/build/linux/halo -> <repository>/assets */
				char *slash;
				int level;

				executable[length] = '\0';
				for (level = 0; level < 3 && (slash = strrchr(executable, '/')); level++)
					*slash = '\0';
				if (level == 3 && strlen(executable) + sizeof("/assets") <= sizeof(executable))
				{
					strcat(executable, "/assets");
					if (has_maps(executable))
						snprintf(root, sizeof(root), "%s", executable);
				}
			}
#ifndef HALO_ANDROID
			if (!has_maps(root) && executable_directory[0] && platform_offer_game_data(executable_directory) &&
				has_maps(executable_directory))
			{
				snprintf(root, sizeof(root), "%s", executable_directory);
			}
#endif
			if (!has_maps(root))
				platform_log("no maps/ folder found; set paths.data in config.toml to the folder that holds maps/");
		}
		trim_separators(root);
		platform_log("data root: %s (the game's log: debug.txt there)", root);
	}
	return root;
}

/* ---------- a dedicated server's maps, checked before it starts */

/* the exit status of a dedicated server without the maps it is to host */
#define DEDICATED_MAPS_EXIT_STATUS 2

/* why the map file name in folder (the data root's maps folder, or
custom_maps; NULL when there is none) cannot be loaded, or NULL when it can:
it is there, and an Xbox cache by its header's signatures and version (5, or
Custom Edition's 609 in custom_maps), which the cache file loader checks first
(cache_file_header_verify) */
static const char *dedicated_map_unusable(const char *folder, const char *file_name, int custom_edition)
{
	char on_disk[256];
	char path[MAX_PATH];
	unsigned char header[0x800];
	unsigned long version;
	FILE *file;
	size_t count;

	if (!folder || !posix_find_entry_case_insensitive(folder, file_name, on_disk, sizeof(on_disk)))
		return "missing";
	snprintf(path, sizeof(path), "%s/%s", folder, on_disk);
	file = fopen(path, "rb");
	if (!file)
		return "not readable";
	count = fread(header, 1, sizeof(header), file);
	fclose(file);
	if (count != sizeof(header) || memcmp(header, "daeh", 4) || memcmp(header + 0x7FC, "toof", 4))
		return "not a map";
	version = header[4] | ((unsigned long)header[5] << 8) | ((unsigned long)header[6] << 16) |
		((unsigned long)header[7] << 24);
	if (version != (custom_edition ? 609UL : 5UL))
		return custom_edition ? "not a Custom Edition map" : "not an Xbox map";
	return NULL;
}

/* whether name starts with prefix, in any case */
static int starts_with_any_case(const char *name, const char *prefix)
{
	for (; *prefix; name++, prefix++)
	{
		if (tolower((unsigned char)*name) != tolower((unsigned char)*prefix))
			return 0;
	}
	return 1;
}

/* server.dedicated: ui.map, which the game loads first, and the maps the
server is to host (debug.network_test's host list, which config_apply_dedicated
makes of server.maps) looked for in the data root before anything else
starts - networking least of all, so that the tunnel port is never bound -
and the game exits with status 2, saying which and where, when any is missing
or is no map. Without them the game would stop at its first map on the damaged
disc screen it shows a player (main_loop_of_death) and stay there, its port bound:
a server that looks healthy, hosting nothing. This way a missing or wrong
volume of maps is a failed start (in Kubernetes, a pod in CrashLoopBackOff).
The maps folder is the one cache_files_map_directory picks (game.language's,
else maps; when that is missing, the first of the others there is). A Custom
Edition map (custom_maps\<name>) is looked for in custom_maps, and left to the
game when a Custom Edition install is set (paths.custom_edition). */
void platform_dedicated_maps_check(void)
{
	static const char *const folders[] = { "maps_de", "maps_fr", "maps_es", "maps_it", "maps" };
	const char *language = config_string("game.language");
	const char *preferred = "maps";
	const char *hosted;
	const char *root;
	char folder_name[256];
	char folder[MAX_PATH];
	char custom_maps[MAX_PATH];
	char looked_for[512];
	char problems[768];
	size_t looked_used = 0;
	size_t problems_used = 0;
	int has_folder;
	int has_custom_maps;
	char seen[512];
	int checked = 0;
	int missing = 0;
	int unusable = 0;
	int index;

	if (!config_boolean("server.dedicated"))
		return;
	root = platform_data_root();
	if (!strncmp(language, "de", 2))
		preferred = "maps_de";
	else if (!strncmp(language, "fr", 2))
		preferred = "maps_fr";
	else if (!strncmp(language, "es", 2))
		preferred = "maps_es";
	else if (!strncmp(language, "it", 2))
		preferred = "maps_it";
	has_folder = posix_find_entry_case_insensitive(root, preferred, folder_name, sizeof(folder_name));
	for (index = 0; !has_folder && index < (int)(sizeof(folders) / sizeof(folders[0])); index++)
		has_folder = posix_find_entry_case_insensitive(root, folders[index], folder_name, sizeof(folder_name));
	snprintf(folder, sizeof(folder), "%s/%s", root, has_folder ? folder_name : preferred);
	has_custom_maps = posix_find_entry_case_insensitive(root, "custom_maps", folder_name, sizeof(folder_name));
	snprintf(custom_maps, sizeof(custom_maps), "%s/%s", root, has_custom_maps ? folder_name : "custom_maps");
	looked_for[0] = 0;
	problems[0] = 0;
	snprintf(seen, sizeof(seen), "|");
	/* ui.map, then each map of the list ("host:a:slayer,king;b:ctf"); a
	server told to join a game instead hosts none */
	hosted = config_string("debug.network_test");
	hosted = !strncmp(hosted, "host:", 5) ? hosted + 5 : "";
	for (index = -1; index < 0 || *hosted; index++)
	{
		char name[128];
		char file_name[160];
		char key[164];
		const char *base;
		const char *reason;
		int custom_edition;

		if (index < 0)
		{
			snprintf(name, sizeof(name), "ui");
		}
		else
		{
			snprintf(name, sizeof(name), "%.*s", (int)strcspn(hosted, ":;"), hosted);
			hosted += strcspn(hosted, ";");
			if (*hosted == ';')
				hosted++;
			if (!name[0])
				continue;
		}
		/* (a level named by its path, a campaign level's for co-op or a
		Custom Edition map's: the file of its last part) */
		base = strrchr(name, '\\');
		base = base ? base + 1 : name;
		custom_edition = starts_with_any_case(name, "custom_maps\\");
		snprintf(file_name, sizeof(file_name), "%s.map", base);
		/* (each file once: a map named again for other game types) */
		snprintf(key, sizeof(key), "|%s|", file_name);
		if (strstr(seen, key) || (custom_edition && platform_custom_edition_root()[0]))
			continue;
		if (strlen(seen) + strlen(key) < sizeof(seen))
			strcat(seen, key + 1);
		checked++;
		if (custom_edition)
			reason = dedicated_map_unusable(has_custom_maps ? custom_maps : NULL, file_name, 1);
		else
			reason = dedicated_map_unusable(has_folder ? folder : NULL, file_name, 0);
		if (looked_used + strlen(file_name) + 3 < sizeof(looked_for))
		{
			looked_used += (size_t)snprintf(looked_for + looked_used, sizeof(looked_for) - looked_used, "%s%s",
				looked_used ? ", " : "", file_name);
		}
		if (!reason)
			continue;
		unusable++;
		if (!strcmp(reason, "missing"))
			missing++;
		if (problems_used + strlen(file_name) + strlen(reason) + 6 < sizeof(problems))
		{
			problems_used += (size_t)snprintf(problems + problems_used, sizeof(problems) - problems_used,
				"%s%s (%s)", problems_used ? ", " : "", file_name, reason);
		}
	}
	if (!unusable)
		return;
	if (missing == checked)
		platform_log("server: no usable maps in %s (looked for %s); exiting", folder, looked_for);
	else
		platform_log("server: maps missing or unusable in %s: %s; exiting", folder, problems);
	exit(DEDICATED_MAPS_EXIT_STATUS);
}

/* creates every missing directory along path */
static void make_directories(const char *path)
{
	char partial[MAX_PATH];
	unsigned long index;

	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index]; index++)
	{
		if (partial[index] == '/')
		{
			partial[index] = '\0';
			if (!directory_exists(partial))
				posix_make_directory(partial);
			partial[index] = '/';
		}
	}
	if (!directory_exists(partial))
		posix_make_directory(partial);
}

const char *platform_save_root(void)
{
	static char root[MAX_PATH];

	if (!root[0])
	{
		const char *environment = config_string("paths.saves");
		const char *data_home = getenv("XDG_DATA_HOME");
		const char *home = getenv("HOME");

		if (*environment)
			snprintf(root, sizeof(root), "%s", environment);
#ifdef _WIN32
		/* the Windows build (port/windows) keeps saves in the roaming
		application data folder */
		else if (getenv("APPDATA") && *getenv("APPDATA"))
			snprintf(root, sizeof(root), "%s/halo", getenv("APPDATA"));
#endif
		else if (data_home && *data_home)
			snprintf(root, sizeof(root), "%s/halo-linux", data_home);
		else if (home && *home)
			snprintf(root, sizeof(root), "%s/.local/share/halo-linux", home);
		else
			snprintf(root, sizeof(root), "%s", platform_data_root());
		trim_separators(root);
		make_directories(root);
		platform_log("save root: %s", root);
	}
	return root;
}

const char *platform_custom_edition_root(void)
{
	static char root[MAX_PATH];
	static int found;

	if (!found)
	{
		char maps[MAX_PATH + 8];
		struct posix_file_information information;

		found = 1;
		snprintf(root, sizeof(root), "%s", config_string("paths.custom_edition"));
		trim_separators(root);
		snprintf(maps, sizeof(maps), "%s/maps", root);
		if (root[0] && posix_stat(maps, &information) != 0)
		{
			platform_log("Halo Custom Edition: %s has no maps folder; not using it", root);
			root[0] = 0;
		}
		if (root[0])
			platform_log("Halo Custom Edition: using the install at %s", root);
	}
	return root;
}

void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size)
{
	char resolved[1024];
	const char *cursor = xbox_path;
	unsigned long length;

	snprintf(resolved, sizeof(resolved), "%s", platform_data_root());
	if (((cursor[0] >= 'a' && cursor[0] <= 'z') || (cursor[0] >= 'A' && cursor[0] <= 'Z')) && cursor[1] == ':')
	{
		char drive = (char)(cursor[0] | 0x20);

		if (drive == 'h' && platform_custom_edition_root()[0])
		{
			snprintf(resolved, sizeof(resolved), "%s", platform_custom_edition_root());
		}
		else if (drive != 'd')
		{
			struct posix_file_information information;

			snprintf(resolved, sizeof(resolved), "%s/%c", platform_save_root(), drive);
			/* every Xbox drive always exists; create its directory on first use */
			if (posix_stat(resolved, &information) != 0)
				posix_make_directory(resolved);
		}
		cursor += 2;
	}

	while (*cursor)
	{
		char component[256];
		char on_disk[256];
		unsigned long component_length = 0;

		while (*cursor == '\\' || *cursor == '/')
			cursor++;
		while (*cursor && *cursor != '\\' && *cursor != '/' && component_length + 1 < sizeof(component))
			component[component_length++] = *cursor++;
		component[component_length] = '\0';
		/* . and .. (and Windows' spellings of them, with trailing dots and
		spaces) are skipped: FATX names are never only dots, and .. would
		leave the root */
		if (!component_length || strspn(component, ". ") == component_length)
			continue;

		if (posix_find_entry_case_insensitive(resolved, component, on_disk, sizeof(on_disk)))
		{
			/* prefer an exact match when several spellings exist */
			char exact[1100];
			struct posix_file_information information;

			snprintf(exact, sizeof(exact), "%s/%s", resolved, component);
			if (posix_stat(exact, &information) == 0)
				snprintf(on_disk, sizeof(on_disk), "%s", component);
		}
		else
		{
			snprintf(on_disk, sizeof(on_disk), "%s", component);
		}
		length = strlen(resolved);
		snprintf(resolved + length, sizeof(resolved) - length, "/%s", on_disk);
	}
	snprintf(host_path, host_path_size, "%s", resolved);
}

/* ---------- file handles */

struct platform_file
{
	int descriptor;
	char path[1024];
};

static void file_destroy(struct platform_handle *handle)
{
	struct platform_file *file = handle->data;

	close(file->descriptor);
	free(file);
}

static struct platform_file *file_from_handle(HANDLE handle)
{
	struct platform_handle *record = platform_handle_get(handle, _platform_handle_file);

	return record ? record->data : NULL;
}

HANDLE WINAPI CreateFileA(LPCSTR file_name, DWORD desired_access, DWORD share_mode,
	LPSECURITY_ATTRIBUTES security_attributes, DWORD creation_disposition,
	DWORD flags_and_attributes, HANDLE template_file)
{
	struct platform_file *file;
	struct platform_handle *handle;
	struct posix_file_information information;
	char path[1024];
	int flags = 0;
	int existed;
	int descriptor;

	(void)share_mode;
	(void)security_attributes;
	(void)flags_and_attributes;
	(void)template_file;

	platform_translate_path(file_name, path, sizeof(path));
	existed = posix_stat(path, &information) == 0;

	if ((desired_access & GENERIC_READ) && (desired_access & GENERIC_WRITE))
		flags = O_RDWR;
	else if (desired_access & GENERIC_WRITE)
		flags = O_WRONLY;
	else
		flags = O_RDONLY;

	switch (creation_disposition)
	{
	case CREATE_NEW:
		flags |= O_CREAT | O_EXCL;
		break;
	case CREATE_ALWAYS:
		flags |= O_CREAT | O_TRUNC;
		break;
	case OPEN_EXISTING:
		break;
	case OPEN_ALWAYS:
		flags |= O_CREAT;
		break;
	case TRUNCATE_EXISTING:
		flags |= O_TRUNC;
		break;
	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if ((flags & O_TRUNC) && (flags & O_ACCMODE) == O_RDONLY)
		flags = (flags & ~O_ACCMODE) | O_RDWR;

	if (existed && (information.flags & _posix_file_is_directory))
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return INVALID_HANDLE_VALUE;
	}

	descriptor = open(path, flags | O_CLOEXEC, 0644);
	if (descriptor < 0)
	{
		platform_set_last_error_from_errno(errno);
		if (errno == ENOENT && !existed && (flags & O_CREAT))
			SetLastError(ERROR_PATH_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}

	file = calloc(1, sizeof(*file));
	handle = file ? platform_handle_new(_platform_handle_file, file, file_destroy) : NULL;
	if (!handle)
	{
		close(descriptor);
		free(file);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	file->descriptor = descriptor;
	snprintf(file->path, sizeof(file->path), "%s", path);

	/* Win32 reports whether an OPEN_ALWAYS/CREATE_ALWAYS target existed */
	SetLastError((existed && (creation_disposition == OPEN_ALWAYS || creation_disposition == CREATE_ALWAYS)) ?
		ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
	return handle;
}

#define READ_BOUNCE_SIZE 0x40000UL

static ssize_t read_some(struct platform_file *file, void *buffer, size_t count, BOOL positioned,
	unsigned long long offset)
{
	return positioned ?
		pread(file->descriptor, buffer, count, (off_t)offset) :
		read(file->descriptor, buffer, count);
}

static BOOL read_at(struct platform_file *file, LPVOID buffer, DWORD count, LPDWORD bytes_read,
	BOOL positioned, unsigned long long offset)
{
	DWORD total = 0;
	/* The renderer write-protects guest memory it caches (memory_watch.c),
	and the kernel fails a read() into a protected page instead of
	faulting. Guest memory is therefore filled through a bounce buffer: the
	copy faults like any other write, at the moment the data really lands,
	so a texture uploaded while the read is in flight is refreshed. */
	BOOL bounce = platform_is_contiguous(buffer) ||
		platform_is_contiguous((char *)buffer + (count ? count - 1 : 0));
	char *staging = bounce ? malloc(count < READ_BOUNCE_SIZE ? count : READ_BOUNCE_SIZE) : NULL;

	if (bounce && !staging)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	while (total < count)
	{
		size_t wanted = count - total;
		ssize_t result;

		if (bounce && wanted > READ_BOUNCE_SIZE)
			wanted = READ_BOUNCE_SIZE;
		result = read_some(file, bounce ? staging : (char *)buffer + total, wanted, positioned, offset + total);
		if (result < 0)
		{
			if (errno == EINTR)
				continue;
			platform_set_last_error_from_errno(errno);
			if (bytes_read)
				*bytes_read = total;
			free(staging);
			return FALSE;
		}
		if (result == 0)
			break;
		if (bounce)
			memcpy((char *)buffer + total, staging, (size_t)result);
		total += (DWORD)result;
	}
	free(staging);
	if (bytes_read)
		*bytes_read = total;
	return TRUE;
}

static BOOL write_at(struct platform_file *file, LPCVOID buffer, DWORD count, LPDWORD bytes_written,
	BOOL positioned, unsigned long long offset)
{
	DWORD total = 0;

	while (total < count)
	{
		ssize_t result = positioned ?
			pwrite(file->descriptor, (const char *)buffer + total, count - total, (off_t)(offset + total)) :
			write(file->descriptor, (const char *)buffer + total, count - total);

		if (result < 0)
		{
			if (errno == EINTR)
				continue;
			platform_set_last_error_from_errno(errno);
			if (bytes_written)
				*bytes_written = total;
			return FALSE;
		}
		total += (DWORD)result;
	}
	if (bytes_written)
		*bytes_written = total;
	return TRUE;
}

static unsigned long long overlapped_offset(LPOVERLAPPED overlapped)
{
	return ((unsigned long long)overlapped->OffsetHigh << 32) | overlapped->Offset;
}

BOOL WINAPI ReadFile(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD bytes_read, LPOVERLAPPED overlapped)
{
	struct platform_file *file = file_from_handle(handle);
	BOOL result;
	DWORD done = 0;

	if (!file)
		return FALSE;
	result = read_at(file, buffer, count, &done, overlapped != NULL, overlapped ? overlapped_offset(overlapped) : 0);
	if (bytes_read)
		*bytes_read = done;
	if (overlapped)
	{
		overlapped->Internal = result ? 0 : GetLastError();
		overlapped->InternalHigh = done;
		if (overlapped->hEvent)
			SetEvent(overlapped->hEvent);
	}
	return result;
}

BOOL WINAPI WriteFile(HANDLE handle, LPCVOID buffer, DWORD count, LPDWORD bytes_written, LPOVERLAPPED overlapped)
{
	struct platform_file *file = file_from_handle(handle);
	BOOL result;
	DWORD done = 0;

	if (!file)
		return FALSE;
	result = write_at(file, buffer, count, &done, overlapped != NULL, overlapped ? overlapped_offset(overlapped) : 0);
	if (bytes_written)
		*bytes_written = done;
	if (overlapped)
	{
		overlapped->Internal = result ? 0 : GetLastError();
		overlapped->InternalHigh = done;
		if (overlapped->hEvent)
			SetEvent(overlapped->hEvent);
	}
	return result;
}

/* ReadFileEx/WriteFileEx complete immediately; as on Win32, the completion
routine runs during the issuing thread's next alertable wait. */

static void file_completion_apc(void *routine, void *overlapped, void *unused)
{
	LPOVERLAPPED request = overlapped;

	(void)unused;
	((LPOVERLAPPED_COMPLETION_ROUTINE)routine)((DWORD)request->Internal, (DWORD)request->InternalHigh, request);
}

BOOL WINAPI ReadFileEx(HANDLE handle, LPVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine)
{
	struct platform_file *file = file_from_handle(handle);
	DWORD done = 0;
	BOOL result;

	if (!file || !overlapped)
	{
		if (file)
			SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	result = read_at(file, buffer, count, &done, TRUE, overlapped_offset(overlapped));
	overlapped->Internal = result ? ERROR_SUCCESS : GetLastError();
	if (result && done == 0 && count > 0)
		overlapped->Internal = ERROR_HANDLE_EOF;
	overlapped->InternalHigh = done;
	platform_queue_apc(file_completion_apc, (void *)completion_routine, overlapped, NULL);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

BOOL WINAPI WriteFileEx(HANDLE handle, LPCVOID buffer, DWORD count, LPOVERLAPPED overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine)
{
	struct platform_file *file = file_from_handle(handle);
	DWORD done = 0;
	BOOL result;

	if (!file || !overlapped)
	{
		if (file)
			SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	result = write_at(file, buffer, count, &done, TRUE, overlapped_offset(overlapped));
	overlapped->Internal = result ? ERROR_SUCCESS : GetLastError();
	overlapped->InternalHigh = done;
	platform_queue_apc(file_completion_apc, (void *)completion_routine, overlapped, NULL);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE handle, LONG distance, PLONG distance_high, DWORD method)
{
	struct platform_file *file = file_from_handle(handle);
	unsigned long low, high;
	int whence;

	if (!file)
		return INVALID_SET_FILE_POINTER;
	switch (method)
	{
	case FILE_BEGIN: whence = SEEK_SET; break;
	case FILE_CURRENT: whence = SEEK_CUR; break;
	case FILE_END: whence = SEEK_END; break;
	default:
		SetLastError(ERROR_INVALID_PARAMETER);
		return INVALID_SET_FILE_POINTER;
	}
	if (posix_seek(file->descriptor, distance,
		distance_high ? *distance_high : (distance < 0 ? -1 : 0), whence, &low, &high) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return INVALID_SET_FILE_POINTER;
	}
	if (distance_high)
		*distance_high = (LONG)high;
	SetLastError(ERROR_SUCCESS);
	return (DWORD)low;
}

DWORD WINAPI GetFileSize(HANDLE handle, LPDWORD size_high)
{
	struct platform_file *file = file_from_handle(handle);
	struct posix_file_information information;

	if (!file)
		return INVALID_FILE_SIZE;
	if (posix_fstat(file->descriptor, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return INVALID_FILE_SIZE;
	}
	if (size_high)
		*size_high = information.size_high;
	SetLastError(ERROR_SUCCESS);
	return information.size_low;
}

BOOL WINAPI SetEndOfFile(HANDLE handle)
{
	struct platform_file *file = file_from_handle(handle);
	unsigned long low, high;

	if (!file)
		return FALSE;
	if (posix_seek(file->descriptor, 0, 0, SEEK_CUR, &low, &high) != 0 ||
		posix_truncate(file->descriptor, low, high) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI GetFileTime(HANDLE handle, LPFILETIME creation_time, LPFILETIME last_access_time, LPFILETIME last_write_time)
{
	struct platform_file *file = file_from_handle(handle);
	struct posix_file_information information;

	if (!file)
		return FALSE;
	if (posix_fstat(file->descriptor, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	if (creation_time)
		platform_unix_time_to_filetime(information.creation_seconds, information.creation_nanoseconds, creation_time);
	if (last_access_time)
		platform_unix_time_to_filetime(information.access_seconds, information.access_nanoseconds, last_access_time);
	if (last_write_time)
		platform_unix_time_to_filetime(information.modification_seconds, information.modification_nanoseconds, last_write_time);
	return TRUE;
}

BOOL WINAPI SetFileTime(HANDLE handle, CONST FILETIME *creation_time, CONST FILETIME *last_access_time,
	CONST FILETIME *last_write_time)
{
	struct platform_file *file = file_from_handle(handle);
	unsigned long access_seconds = 0, access_nanoseconds = 0;
	unsigned long write_seconds = 0, write_nanoseconds = 0;

	(void)creation_time;
	if (!file)
		return FALSE;
	if (last_access_time)
		platform_filetime_to_unix_time(last_access_time, &access_seconds, &access_nanoseconds);
	if (last_write_time)
		platform_filetime_to_unix_time(last_write_time, &write_seconds, &write_nanoseconds);
	if (posix_set_file_times(file->path, access_seconds, access_nanoseconds, write_seconds, write_nanoseconds) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

/* ---------- operations on paths */

static void fill_attribute_data(const struct posix_file_information *information, WIN32_FILE_ATTRIBUTE_DATA *data)
{
	data->dwFileAttributes = 0;
	if (information->flags & _posix_file_is_directory)
		data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
	if (information->flags & _posix_file_is_read_only)
		data->dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
	if (!data->dwFileAttributes)
		data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	platform_unix_time_to_filetime(information->creation_seconds, information->creation_nanoseconds, &data->ftCreationTime);
	platform_unix_time_to_filetime(information->access_seconds, information->access_nanoseconds, &data->ftLastAccessTime);
	platform_unix_time_to_filetime(information->modification_seconds, information->modification_nanoseconds, &data->ftLastWriteTime);
	data->nFileSizeHigh = information->size_high;
	data->nFileSizeLow = information->size_low;
}

DWORD WINAPI GetFileAttributesA(LPCSTR file_name)
{
	struct posix_file_information information;
	WIN32_FILE_ATTRIBUTE_DATA data;
	char path[1024];

	platform_translate_path(file_name, path, sizeof(path));
	if (posix_stat(path, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return (DWORD)-1;
	}
	fill_attribute_data(&information, &data);
	return data.dwFileAttributes;
}

BOOL WINAPI GetFileAttributesExA(LPCSTR file_name, GET_FILEEX_INFO_LEVELS level, LPVOID file_information)
{
	struct posix_file_information information;
	char path[1024];

	if (level != GetFileExInfoStandard)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	platform_translate_path(file_name, path, sizeof(path));
	if (posix_stat(path, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	fill_attribute_data(&information, file_information);
	return TRUE;
}

BOOL WINAPI SetFileAttributesA(LPCSTR file_name, DWORD attributes)
{
	char path[1024];
	struct posix_file_information information;

	platform_translate_path(file_name, path, sizeof(path));
	if (posix_stat(path, &information) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	/* only the read-only bit has a POSIX equivalent */
	if (posix_set_read_only(path, (attributes & FILE_ATTRIBUTE_READONLY) != 0) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI DeleteFileA(LPCSTR file_name)
{
	char path[1024];

	platform_translate_path(file_name, path, sizeof(path));
	if (unlink(path) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI MoveFileA(LPCSTR existing_file_name, LPCSTR new_file_name)
{
	char from[1024], to[1024];
	struct posix_file_information information;

	platform_translate_path(existing_file_name, from, sizeof(from));
	platform_translate_path(new_file_name, to, sizeof(to));
	if (posix_stat(to, &information) == 0)
	{
		SetLastError(ERROR_ALREADY_EXISTS);
		return FALSE;
	}
	if (rename(from, to) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI CopyFileA(LPCSTR existing_file_name, LPCSTR new_file_name, BOOL fail_if_exists)
{
	char from[1024], to[1024];
	char buffer[65536];
	int source, destination;
	BOOL success = TRUE;

	platform_translate_path(existing_file_name, from, sizeof(from));
	platform_translate_path(new_file_name, to, sizeof(to));
	source = open(from, O_RDONLY | O_CLOEXEC);
	if (source < 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	destination = open(to, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
	if (destination < 0)
	{
		platform_set_last_error_from_errno(errno);
		close(source);
		return FALSE;
	}
	for (;;)
	{
		ssize_t count = read(source, buffer, sizeof(buffer));

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
		{
			if (count < 0)
			{
				platform_set_last_error_from_errno(errno);
				success = FALSE;
			}
			break;
		}
		if (write(destination, buffer, (size_t)count) != count)
		{
			platform_set_last_error_from_errno(errno);
			success = FALSE;
			break;
		}
	}
	close(source);
	close(destination);
	return success;
}

BOOL WINAPI CreateDirectoryA(LPCSTR path_name, LPSECURITY_ATTRIBUTES security_attributes)
{
	char path[1024];

	(void)security_attributes;
	platform_translate_path(path_name, path, sizeof(path));
	if (posix_make_directory(path) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI RemoveDirectoryA(LPCSTR path_name)
{
	char path[1024];

	platform_translate_path(path_name, path, sizeof(path));
	if (rmdir(path) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR directory_name, PULARGE_INTEGER free_bytes_available,
	PULARGE_INTEGER total_bytes, PULARGE_INTEGER total_free_bytes)
{
	char path[1024];
	unsigned long free_low, free_high, total_low, total_high;

	platform_translate_path(directory_name ? directory_name : "d:\\", path, sizeof(path));
	if (posix_disk_space(path, &free_low, &free_high, &total_low, &total_high) != 0)
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	if (free_bytes_available)
	{
		free_bytes_available->LowPart = free_low;
		free_bytes_available->HighPart = free_high;
	}
	if (total_free_bytes)
	{
		total_free_bytes->LowPart = free_low;
		total_free_bytes->HighPart = free_high;
	}
	if (total_bytes)
	{
		total_bytes->LowPart = total_low;
		total_bytes->HighPart = total_high;
	}
	return TRUE;
}

/* ---------- directory enumeration */

struct platform_find
{
	void *directory;
	char directory_path[1024];
	char pattern[MAX_PATH];
};

static void find_destroy(struct platform_handle *handle)
{
	struct platform_find *find = handle->data;

	posix_directory_close(find->directory);
	free(find);
}

/* Win32 wildcard match: '*' any run, '?' one character, case-insensitive;
"*.*" also matches names without a dot */
static BOOL wildcard_match(const char *pattern, const char *name)
{
	if (!strcmp(pattern, "*.*") || !strcmp(pattern, "*"))
		return TRUE;
	while (*pattern)
	{
		if (*pattern == '*')
		{
			while (*pattern == '*')
				pattern++;
			if (!*pattern)
				return TRUE;
			for (; *name; name++)
			{
				if (wildcard_match(pattern, name))
					return TRUE;
			}
			return FALSE;
		}
		if (!*name)
			return FALSE;
		if (*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*name))
			return FALSE;
		pattern++;
		name++;
	}
	return *name == '\0';
}

static BOOL find_next_entry(struct platform_find *find, LPWIN32_FIND_DATAA data)
{
	char name[MAX_PATH];

	while (posix_directory_next(find->directory, name, sizeof(name)))
	{
		char full_path[1400];
		struct posix_file_information information;
		WIN32_FILE_ATTRIBUTE_DATA attributes;

		if (!wildcard_match(find->pattern, name))
			continue;
		snprintf(full_path, sizeof(full_path), "%s/%s", find->directory_path, name);
		if (posix_stat(full_path, &information) != 0)
			continue;
		fill_attribute_data(&information, &attributes);
		memset(data, 0, sizeof(*data));
		data->dwFileAttributes = attributes.dwFileAttributes;
		data->ftCreationTime = attributes.ftCreationTime;
		data->ftLastAccessTime = attributes.ftLastAccessTime;
		data->ftLastWriteTime = attributes.ftLastWriteTime;
		data->nFileSizeHigh = attributes.nFileSizeHigh;
		data->nFileSizeLow = attributes.nFileSizeLow;
		snprintf(data->cFileName, sizeof(data->cFileName), "%s", name);
		return TRUE;
	}
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE WINAPI FindFirstFileA(LPCSTR file_name, LPWIN32_FIND_DATAA data)
{
	struct platform_find *find = calloc(1, sizeof(*find));
	struct platform_handle *handle;
	char xbox_directory[MAX_PATH];
	const char *separator = strrchr(file_name, '\\');

	if (!find)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	if (!separator)
		separator = strrchr(file_name, '/');
	if (separator)
	{
		snprintf(xbox_directory, sizeof(xbox_directory), "%.*s", (int)(separator - file_name), file_name);
		snprintf(find->pattern, sizeof(find->pattern), "%s", separator + 1);
	}
	else
	{
		xbox_directory[0] = '\0';
		snprintf(find->pattern, sizeof(find->pattern), "%s", file_name);
	}
	platform_translate_path(xbox_directory, find->directory_path, sizeof(find->directory_path));
	find->directory = posix_directory_open(find->directory_path);
	if (!find->directory)
	{
		platform_set_last_error_from_errno(errno);
		free(find);
		return INVALID_HANDLE_VALUE;
	}
	handle = platform_handle_new(_platform_handle_find, find, find_destroy);
	if (!handle)
	{
		posix_directory_close(find->directory);
		free(find);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	if (!find_next_entry(find, data))
	{
		CloseHandle(handle);
		SetLastError(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	return handle;
}

BOOL WINAPI FindNextFileA(HANDLE find_file, LPWIN32_FIND_DATAA data)
{
	struct platform_handle *handle = platform_handle_get(find_file, _platform_handle_find);

	if (!handle)
		return FALSE;
	return find_next_entry(handle->data, data);
}

/* FindClose is an XDK macro for CloseHandle */
