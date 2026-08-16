/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * stubgen.cpp — identity stubs for per-window helper teams (H3).
 *
 * Deskbar derives an application row's label from the team binary's FILE
 * NAME (BarApp.cpp:931), its icon from that binary's attributes
 * (BAppFileInfo, attributes-only for a plain ELF) and it groups teams by
 * signature — so a guest app gets its own row exactly when its helper runs
 * from a binary named after the app and registered under a unique
 * signature. This file builds those binaries: a byte copy of
 * vitrine_window_host in ~/config/cache/vitrine/apps/<Name>, stamped with
 * BEOS:APP_SIG + SYS:NAME, plus the BeOS icon attributes copied verbatim
 * from the app's real executable WHEN it carries any. (Today Debian
 * binaries don't; once upstream ships an icon-stamping mechanism the
 * copies pick the icons up automatically — nothing here depends on it.)
 *
 * Identity resolution follows the freedesktop convention: the Wayland
 * app_id names the .desktop file; the fallback scan matches
 * StartupWMClass= or the Exec= basename. No .desktop at all still yields
 * a stub — named after the app_id — because the per-app team split (and
 * with it same-team dialog stacking) must not hinge on packaging quality.
 *
 * Staleness: VOS:WH_SRC on every stub records the helper binary identity
 * (dev:ino:mtime:size) plus app_id and .desktop mtime. A full match is
 * reused, a mismatch rebuilt in place (tmp + rename, so a still-running
 * helper keeps its old inode), and vitrine_stub_gc() unlinks stubs from
 * older helper builds at startup.
 */
#include "stubgen.h"

#include <fs_attr.h>

#include <Node.h>
#include <TypeConstants.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

static const char* kSrcAttr = "VOS:WH_SRC";
static const char* kIconAttrs[] = {
	"BEOS:ICON",		/* HVIF vector */
	"BEOS:L:STD_ICON",	/* 32x32 B_CMAP8 */
	"BEOS:M:STD_ICON",	/* 16x16 B_CMAP8 */
};

struct DesktopIdentity {
	std::string path;	/* the .desktop file */
	std::string name;	/* Name= */
	std::string exec;	/* Exec= (raw line) */
};


static std::string
leaf_name(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}


/* Parse the [Desktop Entry] group. When app_id is non-empty the file
 * counts as a match only if StartupWMClass= or the Exec= basename equals
 * it (the direct <app_id>.desktop hit passes an empty app_id). */
static bool
parse_desktop(const std::string& path, const std::string& app_id,
	DesktopIdentity& out)
{
	FILE* f = fopen(path.c_str(), "r");
	if (f == NULL)
		return false;

	bool in_entry = false;
	bool matches = app_id.empty();
	std::string name, exec;

	char line[1024];
	while (fgets(line, sizeof(line), f) != NULL) {
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();

		if (!s.empty() && s[0] == '[') {
			/* Desktop Actions repeat Exec= lines; only the main group
			 * may satisfy the match. */
			in_entry = (s == "[Desktop Entry]");
			continue;
		}
		if (!in_entry)
			continue;

		size_t eq = s.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = s.substr(0, eq);
		std::string value = s.substr(eq + 1);

		if (key == "Name")
			name = value;
		else if (key == "Exec") {
			if (exec.empty())
				exec = value;
			std::string first = value.substr(0, value.find(' '));
			if (!app_id.empty() && leaf_name(first) == app_id)
				matches = true;
		} else if (key == "StartupWMClass" && value == app_id)
			matches = true;
	}
	fclose(f);

	if (!matches)
		return false;
	out.path = path;
	out.name = name;
	out.exec = exec;
	return true;
}


static const char* kDesktopDirs[] = {
	"/usr/share/applications",
	"/usr/local/share/applications",
};


static bool
find_desktop(const std::string& app_id, DesktopIdentity& out)
{
	/* Freedesktop convention first: app_id == desktop file basename. */
	for (const char* dir : kDesktopDirs) {
		std::string direct = std::string(dir) + "/" + app_id + ".desktop";
		if (access(direct.c_str(), R_OK) == 0
				&& parse_desktop(direct, "", out))
			return true;
	}
	for (const char* dir : kDesktopDirs) {
		DIR* d = opendir(dir);
		if (d == NULL)
			continue;
		struct dirent* ent;
		while ((ent = readdir(d)) != NULL) {
			std::string fname(ent->d_name);
			if (fname.size() < 9
				|| fname.compare(fname.size() - 8, 8, ".desktop") != 0)
				continue;
			if (parse_desktop(std::string(dir) + "/" + fname, app_id,
					out)) {
				closedir(d);
				return true;
			}
		}
		closedir(d);
	}
	return false;
}


/* The Exec= line's first word, resolved to an absolute path — the binary
 * whose (possible) icon attributes the stub inherits. Interpreter
 * launchers hide the real app behind arguments; give up on those. */
static std::string
resolve_exec(const std::string& exec)
{
	std::string first = exec.substr(0, exec.find(' '));
	if (first.empty())
		return std::string();
	std::string leaf = leaf_name(first);
	if (leaf == "sh" || leaf == "bash" || leaf == "dash" || leaf == "env")
		return std::string();
	if (first[0] == '/')
		return access(first.c_str(), X_OK) == 0 ? first : std::string();
	static const char* kBinDirs[] = {"/usr/bin", "/usr/local/bin", "/bin"};
	for (const char* dir : kBinDirs) {
		std::string path = std::string(dir) + "/" + leaf;
		if (access(path.c_str(), X_OK) == 0)
			return path;
	}
	return std::string();
}


/* File-name / signature sanitizers. The label may keep spaces (BeOS file
 * names do); only path separators are replaced. Signatures stay in the
 * conservative MIME charset. */
static std::string
sanitize_label(const std::string& s)
{
	std::string out = s;
	for (char& c : out) {
		if (c == '/' || c == '\n' || c == '\r')
			c = '-';
	}
	return out;
}


static std::string
sanitize_sig_part(const std::string& s)
{
	std::string out = s;
	for (char& c : out) {
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '.' || c == '-'
			|| c == '_' || c == '+';
		if (!ok)
			c = '-';
	}
	return out;
}


/* Helper-binary identity for the staleness tag; "" on stat failure (which
 * then never matches, forcing a rebuild — the safe direction). */
static std::string
helper_identity(const char* helper_path)
{
	struct stat st;
	if (stat(helper_path, &st) != 0)
		return std::string();
	char buf[128];
	snprintf(buf, sizeof(buf), "%llx:%llx:%llx:%llx",
		(unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
		(unsigned long long)st.st_mtime, (unsigned long long)st.st_size);
	return std::string(buf);
}


static std::string
stub_dir(void)
{
	const char* home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return std::string();
	return std::string(home) + "/config/cache/vitrine/apps";
}


static bool
mkdir_p(const std::string& path)
{
	std::string partial;
	size_t pos = 0;
	while (pos != std::string::npos) {
		pos = path.find('/', pos + 1);
		partial = path.substr(0, pos);
		if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST)
			return false;
	}
	return true;
}


static std::string
read_attr_string(BNode& node, const char* attr)
{
	attr_info info;
	if (node.GetAttrInfo(attr, &info) != B_OK || info.size <= 0
			|| info.size > 4096)
		return std::string();
	std::string value((size_t)info.size, '\0');
	ssize_t got = node.ReadAttr(attr, info.type, 0, &value[0],
		(size_t)info.size);
	if (got <= 0)
		return std::string();
	value.resize(strnlen(value.c_str(), (size_t)got));
	return value;
}


static bool
copy_file(const std::string& src, const std::string& dst)
{
	int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return false;
	std::string tmp = dst + ".tmp";
	int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		0755);
	if (out < 0) {
		close(in);
		return false;
	}

	char buf[65536];
	ssize_t n;
	bool ok = true;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			ok = false;
			break;
		}
	}
	if (n < 0)
		ok = false;
	close(in);
	if (close(out) != 0)
		ok = false;
	/* rename over: a running helper keeps its old inode (no ETXTBSY). */
	if (ok && rename(tmp.c_str(), dst.c_str()) != 0)
		ok = false;
	if (!ok)
		unlink(tmp.c_str());
	return ok;
}


/* Copy one attribute verbatim if the source has it. */
static void
copy_attr(BNode& from, BNode& to, const char* attr)
{
	attr_info info;
	if (from.GetAttrInfo(attr, &info) != B_OK || info.size <= 0
			|| info.size > 65536)
		return;
	std::string buf((size_t)info.size, '\0');
	if (from.ReadAttr(attr, info.type, 0, &buf[0], (size_t)info.size)
			!= (ssize_t)info.size)
		return;
	to.WriteAttr(attr, info.type, 0, buf.data(), (size_t)info.size);
}


char *
vitrine_stub_for_app(const char *app_id, const char *helper_path,
	char **sig_out)
{
	if (app_id == NULL || app_id[0] == '\0' || sig_out == NULL)
		return NULL;
	std::string dir = stub_dir();
	if (dir.empty() || !mkdir_p(dir))
		return NULL;

	DesktopIdentity ident;
	bool have_desktop = find_desktop(app_id, ident);

	std::string label = sanitize_label(
		have_desktop && !ident.name.empty() ? ident.name : app_id);
	std::string sig = "application/x-vnd.vos.wh-"
		+ sanitize_sig_part(app_id);
	std::string path = dir + "/" + label;

	std::string desktop_tag = "0";
	if (have_desktop) {
		struct stat st;
		if (stat(ident.path.c_str(), &st) == 0) {
			char buf[32];
			snprintf(buf, sizeof(buf), "%llx",
				(unsigned long long)st.st_mtime);
			desktop_tag = buf;
		}
	}
	std::string src_tag = helper_identity(helper_path) + "|" + app_id + "|"
		+ desktop_tag;

	/* Reuse only on a full identity match. A label collision between two
	 * app_ids surfaces as an app_id mismatch here and falls back to the
	 * app_id as the file name — ugly label, correct identity. */
	{
		BNode existing(path.c_str());
		if (existing.InitCheck() == B_OK) {
			std::string tag = read_attr_string(existing, kSrcAttr);
			if (tag == src_tag) {
				*sig_out = strdup(sig.c_str());
				return strdup(path.c_str());
			}
			size_t first = tag.find('|');
			size_t last = tag.rfind('|');
			if (first != std::string::npos && last > first
					&& tag.substr(first + 1, last - first - 1)
						!= app_id) {
				label = sanitize_label(app_id);
				path = dir + "/" + label;
				src_tag = helper_identity(helper_path) + "|" + app_id
					+ "|" + desktop_tag;
			}
		}
	}

	if (!copy_file(helper_path, path))
		return NULL;

	BNode node(path.c_str());
	if (node.InitCheck() != B_OK) {
		unlink(path.c_str());
		return NULL;
	}

	/* Wire formats per BAppFileInfo: MIME string with trailing NUL;
	 * SYS:NAME is "<sig minus application/>:<context>:<label>" — with no
	 * catalog installed the label comes back verbatim. */
	node.WriteAttr("BEOS:APP_SIG", B_MIME_STRING_TYPE, 0, sig.c_str(),
		sig.size() + 1);
	std::string sys_name = sig.substr(strlen("application/"))
		+ ":System name:" + label;
	node.WriteAttr("SYS:NAME", B_STRING_TYPE, 0, sys_name.c_str(),
		sys_name.size() + 1);

	if (have_desktop) {
		std::string binary = resolve_exec(ident.exec);
		if (!binary.empty()) {
			BNode source(binary.c_str());
			if (source.InitCheck() == B_OK) {
				for (const char* attr : kIconAttrs)
					copy_attr(source, node, attr);
			}
		}
	}

	/* Last: an interrupted build leaves no tag and is rebuilt next time. */
	node.WriteAttr(kSrcAttr, B_STRING_TYPE, 0, src_tag.c_str(),
		src_tag.size() + 1);

	*sig_out = strdup(sig.c_str());
	return strdup(path.c_str());
}


void
vitrine_stub_gc(const char *helper_path)
{
	std::string dir = stub_dir();
	if (dir.empty())
		return;
	std::string current = helper_identity(helper_path);

	DIR* d = opendir(dir.c_str());
	if (d == NULL)
		return;
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		std::string path = dir + "/" + ent->d_name;
		BNode node(path.c_str());
		if (node.InitCheck() != B_OK)
			continue;
		std::string tag = read_attr_string(node, kSrcAttr);
		size_t sep = tag.find('|');
		if (current.empty() || sep == std::string::npos
				|| tag.substr(0, sep) != current)
			unlink(path.c_str());
	}
	closedir(d);
}
