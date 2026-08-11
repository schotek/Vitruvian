/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * appstamp — stamp BeOS application attributes onto a Linux binary from its
 * freedesktop metadata, so Deskbar and Tracker show the application's real
 * icon instead of the generic one.
 *
 * Debian binaries carry no BeOS resources or attributes; their name and icon
 * live in /usr/share/applications/<app>.desktop and the icon theme. Deskbar
 * resolves both purely from the executable's attributes (BAppFileInfo in
 * attributes-only mode for a plain ELF), so bridging is a matter of writing
 * the right xattrs:
 *
 *   BEOS:M:STD_ICON  16x16 B_CMAP8   ('MICN')
 *   BEOS:L:STD_ICON  32x32 B_CMAP8   ('ICON')
 *   BEOS:ICON        HVIF vector     ('VICN', only with --hvif)
 *   BEOS:APP_SIG     MIME signature  — the QPA plugin picks this up too
 *   SYS:NAME         catalog entry   — Deskbar resolves the label while the
 *                                      application is RUNNING, so FindApp(sig)
 *                                      succeeds via the roster and BCatalog
 *                                      falls back to the key itself: the
 *                                      label works without any catalog
 *
 * dpkg does not track xattrs, so the package content stays pristine.
 *
 * WHY A STAMPING TOOL AT ALL — and what could replace it.
 *
 * Every desktop answers "how does the shell know a window's icon"
 * differently. On X11 the application PUSHES: Qt's xcb backend implements
 * QPlatformWindow::setWindowIcon() by setting _NET_WM_ICON on the window,
 * and the taskbar reads it from there. On Wayland the shell PULLS: a
 * toplevel carries only an app_id, and the compositor looks up
 * <app_id>.desktop and the icon theme itself (xdg-toplevel-icon-v1 adds a
 * push path, but .desktop matching remains the norm). On BeOS the shell
 * pulls too, but from the EXECUTABLE FILE: Deskbar goes signature ->
 * be_roster -> binary -> BAppFileInfo, and native applications carry their
 * icon in resources from the day they are compiled.
 *
 * Qt applications push their icon into our QPA plugin exactly as they do
 * on X11 — QGuiApplication::setWindowIcon() ends up calling the plugin's
 * QPlatformWindow::setWindowIcon(). The plugin drops it, because there is
 * no receiving end: BWindow has no icon API, TBarApp::MessageReceived()
 * accepts no per-team icon message, and the plugin (running as the session
 * user) cannot stamp a root-owned binary. The icon arrives and dies.
 *
 * So the clean replacement for the Deskbar half of this tool is a runtime
 * channel — an _NET_WM_ICON analogue: teach Deskbar (or the registrar) a
 * "set icon/name for this team" message and implement setWindowIcon() in
 * the QPA plugin to send it. Debian Qt applications would then get their
 * icons with no stamping at all, exactly like on X11. That is a platform
 * behaviour change, i.e. an upstream conversation, not something to slip
 * into a bridge tool.
 *
 * Even then this tool keeps the file-side half of the job: Tracker shows
 * a binary's icon while the application is NOT running (no runtime channel
 * can help there), and applications that never call setWindowIcon still
 * need their metadata from somewhere.
 *
 * Deliberately NOT a BApplication and deliberately no BBitmap: both need a
 * live app_server, and this tool must run from a root ssh session. PNG
 * loading is libpng's simplified API; CMAP8 quantization is a direct
 * nearest-colour search over the static system palette
 * (headers/private/interface/Palette.h).
 */
#include <GraphicsDefs.h>
#include <Mime.h>
#include <Node.h>
#include <TypeConstants.h>

#include <Palette.h>

#include <png.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>


static const char* kMiniAttr = "BEOS:M:STD_ICON";
static const char* kLargeAttr = "BEOS:L:STD_ICON";
static const char* kVectorAttr = "BEOS:ICON";

// The ext4 xattr value ceiling is one block minus overhead; the nexus layer
// adds a 4-byte type prefix and does no chunking. Anything near 4K is refused
// by the filesystem anyway — check early for a friendlier message.
static const size_t kMaxAttrPayload = 4000;


struct DesktopEntry {
	std::string path;
	std::string name;
	std::string icon;
};


static std::string
leafName(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}


// First word of an Exec= line, stripped of its directory part. Quoting and
// field codes (%f, %u) never appear in the first word of well-formed entries.
static std::string
execBasename(const std::string& execLine)
{
	size_t space = execLine.find(' ');
	return leafName(execLine.substr(0, space));
}


// With an empty wantedLeaf the Exec/TryExec match is not required — used for
// an explicitly given .desktop file, which the caller has already chosen.
static bool
parseDesktopFile(const std::string& path, DesktopEntry& entry,
	const std::string& wantedLeaf)
{
	FILE* f = fopen(path.c_str(), "r");
	if (f == NULL)
		return false;

	bool inDesktopEntry = false;
	bool matches = wantedLeaf.empty();
	std::string name, icon;

	char line[1024];
	while (fgets(line, sizeof(line), f) != NULL) {
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();

		if (!s.empty() && s[0] == '[') {
			// Only the main group holds the keys we want; Desktop Actions
			// repeat Exec= lines that must not cause false matches.
			inDesktopEntry = (s == "[Desktop Entry]");
			continue;
		}
		if (!inDesktopEntry)
			continue;

		size_t eq = s.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = s.substr(0, eq);
		std::string value = s.substr(eq + 1);

		if (key == "Name")
			name = value;
		else if (key == "Icon")
			icon = value;
		else if (key == "TryExec" || key == "Exec") {
			if (execBasename(value) == wantedLeaf)
				matches = true;
		} else if (key == "NoDisplay" && value == "true"
				&& !wantedLeaf.empty()) {
			// Hidden entries (kcm modules etc.) are poor name sources when
			// searching; an explicitly given file is the caller's choice.
			matches = false;
			break;
		}
	}
	fclose(f);

	if (!matches)
		return false;
	entry.path = path;
	entry.name = name;
	entry.icon = icon;
	return true;
}


static bool
findDesktopFor(const std::string& binary, DesktopEntry& entry)
{
	const std::string leaf = leafName(binary);
	static const char* kDirs[] = {
		"/usr/share/applications",
		"/usr/local/share/applications",
	};

	for (const char* dirPath : kDirs) {
		DIR* dir = opendir(dirPath);
		if (dir == NULL)
			continue;
		struct dirent* ent;
		while ((ent = readdir(dir)) != NULL) {
			std::string fname(ent->d_name);
			if (fname.size() < 9
				|| fname.compare(fname.size() - 8, 8, ".desktop") != 0)
				continue;
			if (parseDesktopFile(std::string(dirPath) + "/" + fname, entry,
					leaf)) {
				closedir(dir);
				return true;
			}
		}
		closedir(dir);
	}
	return false;
}


// ---- PNG loading and scaling ----------------------------------------------

struct Image {
	uint32 width = 0;
	uint32 height = 0;
	std::vector<uint8> rgba;	// width * height * 4
};


static bool
loadPng(const std::string& path, Image& img)
{
	png_image png;
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_file(&png, path.c_str()))
		return false;
	png.format = PNG_FORMAT_RGBA;
	img.width = png.width;
	img.height = png.height;
	img.rgba.resize(PNG_IMAGE_SIZE(png));
	if (!png_image_finish_read(&png, NULL, img.rgba.data(), 0, NULL)) {
		png_image_free(&png);
		return false;
	}
	return true;
}


// Box filter, correct for both down- and upscaling at these tiny sizes.
// Icons are 16/32 px; nothing here is worth pulling a resampling library for.
static Image
scaleTo(const Image& src, uint32 size)
{
	Image dst;
	dst.width = size;
	dst.height = size;
	dst.rgba.resize(size * size * 4);

	for (uint32 y = 0; y < size; y++) {
		uint32 sy0 = y * src.height / size;
		uint32 sy1 = (y + 1) * src.height / size;
		if (sy1 <= sy0)
			sy1 = sy0 + 1;
		for (uint32 x = 0; x < size; x++) {
			uint32 sx0 = x * src.width / size;
			uint32 sx1 = (x + 1) * src.width / size;
			if (sx1 <= sx0)
				sx1 = sx0 + 1;

			uint32 sum[4] = {0, 0, 0, 0};
			uint32 count = 0;
			for (uint32 sy = sy0; sy < sy1 && sy < src.height; sy++) {
				const uint8* p = &src.rgba[(sy * src.width + sx0) * 4];
				for (uint32 sx = sx0; sx < sx1 && sx < src.width; sx++) {
					sum[0] += p[0];
					sum[1] += p[1];
					sum[2] += p[2];
					sum[3] += p[3];
					p += 4;
					count++;
				}
			}
			uint8* d = &dst.rgba[(y * size + x) * 4];
			for (int c = 0; c < 4; c++)
				d[c] = count > 0 ? sum[c] / count : 0;
		}
	}
	return dst;
}


// Icon theme lookup: prefer the exact size, then the smallest larger art
// (downscales cleanly), then the largest smaller one, then pixmaps.
static bool
findThemePng(const std::string& iconName, uint32 size, std::string& path)
{
	if (!iconName.empty() && iconName[0] == '/') {
		path = iconName;
		FILE* f = fopen(path.c_str(), "r");
		if (f != NULL) {
			fclose(f);
			return true;
		}
		return false;
	}

	static const uint32 kSizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
	std::vector<uint32> order;
	order.push_back(size);
	for (uint32 s : kSizes)
		if (s > size)
			order.push_back(s);
	for (int i = sizeof(kSizes) / sizeof(kSizes[0]) - 1; i >= 0; i--)
		if (kSizes[i] < size)
			order.push_back(kSizes[i]);

	char buf[512];
	for (uint32 s : order) {
		snprintf(buf, sizeof(buf),
			"/usr/share/icons/hicolor/%ux%u/apps/%s.png", s, s,
			iconName.c_str());
		FILE* f = fopen(buf, "r");
		if (f != NULL) {
			fclose(f);
			path = buf;
			return true;
		}
	}
	snprintf(buf, sizeof(buf), "/usr/share/pixmaps/%s.png", iconName.c_str());
	FILE* f = fopen(buf, "r");
	if (f != NULL) {
		fclose(f);
		path = buf;
		return true;
	}
	return false;
}


// Nearest colour in the static system palette. PaletteConverter would do
// this, but its lookup methods are inline-defined inside libbe and not
// exported; for an icon's ~1K pixels a direct search is instant anyway.
static uint8
nearestPaletteIndex(uint8 r, uint8 g, uint8 b)
{
	uint32 best = UINT32_MAX;
	uint8 bestIndex = 0;
	for (int i = 0; i < 256; i++) {
		const rgb_color& c = kSystemPalette[i];
		// Weighted euclidean; the eye is most sensitive to green.
		int32 dr = (int32)c.red - r;
		int32 dg = (int32)c.green - g;
		int32 db = (int32)c.blue - b;
		uint32 d = 3 * dr * dr + 6 * dg * dg + 2 * db * db;
		if (d < best) {
			best = d;
			bestIndex = (uint8)i;
		}
	}
	return bestIndex;
}


// SVG-only themes (featherpad ships nothing but scalable/): rasterize via
// rsvg-convert when it is installed. Kept external on purpose — pulling an
// SVG renderer into the tree for a stamping tool would be out of proportion.
static bool
rasterizeSvg(const std::string& iconName, uint32 size, Image& img)
{
	char svgPath[512];
	snprintf(svgPath, sizeof(svgPath),
		"/usr/share/icons/hicolor/scalable/apps/%s.svg", iconName.c_str());
	FILE* probe = fopen(svgPath, "r");
	if (probe == NULL)
		return false;
	fclose(probe);

	char tmpPath[64];
	snprintf(tmpPath, sizeof(tmpPath), "/tmp/appstamp-%d-%u.png",
		(int)getpid(), size);
	char cmd[1200];
	snprintf(cmd, sizeof(cmd),
		"rsvg-convert -w %u -h %u -o %s '%s' 2>/dev/null", size, size,
		tmpPath, svgPath);
	int rc = system(cmd);
	if (rc != 0) {
		fprintf(stderr, "  found %s but rsvg-convert is unavailable —"
			" install librsvg2-bin\n", svgPath);
		return false;
	}
	bool ok = loadPng(tmpPath, img);
	unlink(tmpPath);
	return ok;
}


static std::vector<uint8>
quantizeToCMAP8(const Image& img)
{
	std::vector<uint8> out(img.width * img.height);
	const uint8* p = img.rgba.data();
	for (size_t i = 0; i < out.size(); i++, p += 4) {
		out[i] = p[3] < 128
			? B_TRANSPARENT_MAGIC_CMAP8
			: nearestPaletteIndex(p[0], p[1], p[2]);
	}
	return out;
}


// ---- stamping --------------------------------------------------------------

static status_t
writeIconAttr(BNode& node, const char* attr, type_code type, uint32 size,
	const Image& source, bool dryRun)
{
	Image scaled = (source.width == size && source.height == size)
		? source : scaleTo(source, size);
	std::vector<uint8> cmap = quantizeToCMAP8(scaled);

	if (dryRun) {
		printf("  would write %s (%lu bytes)\n", attr, (unsigned long)cmap.size());
		return B_OK;
	}
	ssize_t written = node.WriteAttr(attr, type, 0, cmap.data(), cmap.size());
	if (written < 0)
		return (status_t)written;
	printf("  %s: %ux%u B_CMAP8\n", attr, size, size);
	return B_OK;
}


static void
usage(FILE* out)
{
	fputs("usage: appstamp [options] <binary>\n"
		"Stamp BeOS attributes (icon, signature, catalog name) onto a Linux\n"
		"binary from its freedesktop .desktop metadata.\n\n"
		"  --desktop <file>    use this .desktop instead of searching\n"
		"  --signature <mime>  override the MIME signature\n"
		"  --name <text>       override the display name\n"
		"  --hvif <file>       also stamp BEOS:ICON from an HVIF file\n"
		"  --dry-run           report without writing\n"
		"  --remove            remove previously stamped attributes\n", out);
}


int
main(int argc, char** argv)
{
	std::string binary, desktopPath, signature, name, hvifPath;
	bool dryRun = false;
	bool remove = false;

	for (int i = 1; i < argc; i++) {
		std::string arg(argv[i]);
		if (arg == "--desktop" && i + 1 < argc)
			desktopPath = argv[++i];
		else if (arg == "--signature" && i + 1 < argc)
			signature = argv[++i];
		else if (arg == "--name" && i + 1 < argc)
			name = argv[++i];
		else if (arg == "--hvif" && i + 1 < argc)
			hvifPath = argv[++i];
		else if (arg == "--dry-run")
			dryRun = true;
		else if (arg == "--remove")
			remove = true;
		else if (arg == "--help" || arg == "-h") {
			usage(stdout);
			return 0;
		} else if (!arg.empty() && arg[0] == '-') {
			fprintf(stderr, "appstamp: unknown option %s\n", arg.c_str());
			usage(stderr);
			return 1;
		} else
			binary = arg;
	}

	if (binary.empty()) {
		usage(stderr);
		return 1;
	}

	BNode node(binary.c_str());
	if (node.InitCheck() != B_OK) {
		fprintf(stderr, "appstamp: cannot open %s\n", binary.c_str());
		return 1;
	}

	if (remove) {
		static const char* kAll[] = {kMiniAttr, kLargeAttr, kVectorAttr,
			"BEOS:APP_SIG", "SYS:NAME"};
		for (const char* attr : kAll) {
			if (node.RemoveAttr(attr) == B_OK)
				printf("  removed %s\n", attr);
		}
		return 0;
	}

	// Metadata: explicit .desktop (no Exec match required — the caller chose
	// the file), or search /usr/share/applications by the binary's leaf name.
	DesktopEntry entry;
	bool haveDesktop = false;
	if (!desktopPath.empty()) {
		haveDesktop = parseDesktopFile(desktopPath, entry, "");
		if (!haveDesktop) {
			fprintf(stderr, "appstamp: cannot read %s\n", desktopPath.c_str());
			return 1;
		}
	} else {
		haveDesktop = findDesktopFor(binary, entry);
	}

	if (haveDesktop)
		printf("%s: using %s\n", binary.c_str(), entry.path.c_str());
	else if (name.empty() && hvifPath.empty()) {
		fprintf(stderr, "appstamp: no .desktop entry found for %s\n"
			"(searched /usr/share/applications; use --desktop, or --name/"
			"--hvif to stamp explicitly)\n", binary.c_str());
		return 1;
	}

	const std::string leaf = leafName(binary);
	if (signature.empty())
		signature = "application/x-vnd.qt6-" + leaf;
	if (name.empty())
		name = entry.name.empty() ? leaf : entry.name;

	// Raster icons: theme PNGs first, SVG rasterization as fallback.
	if (!entry.icon.empty()) {
		std::string png32, png16;
		Image img32, img16;
		bool have32 = false, have16 = false;

		if (findThemePng(entry.icon, 32, png32) && loadPng(png32, img32)) {
			have32 = true;
			printf("  icon source: %s (%ux%u)\n", png32.c_str(),
				img32.width, img32.height);
			// A separate 16px file usually exists and looks better than a
			// downscale of the large art; fall back to scaling img32.
			have16 = findThemePng(entry.icon, 16, png16) && png16 != png32
				&& loadPng(png16, img16);
		} else if (rasterizeSvg(entry.icon, 32, img32)) {
			have32 = true;
			printf("  icon source: scalable SVG via rsvg-convert\n");
			have16 = rasterizeSvg(entry.icon, 16, img16);
		}

		if (have32) {
			writeIconAttr(node, kLargeAttr, B_LARGE_ICON_TYPE, 32, img32,
				dryRun);
			writeIconAttr(node, kMiniAttr, B_MINI_ICON_TYPE, 16,
				have16 ? img16 : img32, dryRun);
		} else {
			fprintf(stderr, "  no usable icon for '%s'\n",
				entry.icon.c_str());
		}
	}

	// Optional crisp vector icon.
	if (!hvifPath.empty()) {
		FILE* f = fopen(hvifPath.c_str(), "rb");
		if (f == NULL) {
			fprintf(stderr, "appstamp: cannot read %s\n", hvifPath.c_str());
			return 1;
		}
		std::vector<uint8> hvif;
		uint8 buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			hvif.insert(hvif.end(), buf, buf + n);
		fclose(f);
		if (hvif.size() > kMaxAttrPayload) {
			fprintf(stderr, "appstamp: %s is %zu bytes; attribute values are"
				" capped near 4K (single ext4 xattr, no chunking)\n",
				hvifPath.c_str(), hvif.size());
			return 1;
		}
		if (dryRun)
			printf("  would write %s (%zu bytes)\n", kVectorAttr, hvif.size());
		else if (node.WriteAttr(kVectorAttr, B_VECTOR_ICON_TYPE, 0,
				hvif.data(), hvif.size()) >= 0)
			printf("  %s: HVIF, %zu bytes\n", kVectorAttr, hvif.size());
	}

	// Signature and catalog name through the same BNode as the icons.
	// Deliberately NOT BAppFileInfo: that needs a B_READ_WRITE BFile, which
	// fails with ETXTBSY while the application is running — attribute writes
	// only need write permission on the file, not its data fork open.
	// Wire format matches BAppFileInfo (NUL included in the payload).
	//
	// SYS:NAME is <sig without "application/">:<context>:<string>. Deskbar
	// resolves the label while the application is running, so FindApp(sig)
	// succeeds via the roster and BCatalog::GetString falls back to the
	// key itself — the untranslated name — with no catalog installed.
	std::string sigLeaf = signature;
	if (sigLeaf.rfind("application/", 0) == 0)
		sigLeaf = sigLeaf.substr(strlen("application/"));
	std::string catalogEntry = sigLeaf + ":System name:" + name;

	if (dryRun) {
		printf("  would set signature %s\n", signature.c_str());
		printf("  would set SYS:NAME %s\n", catalogEntry.c_str());
		return 0;
	}

	if (node.WriteAttr("BEOS:APP_SIG", B_MIME_STRING_TYPE, 0,
			signature.c_str(), signature.size() + 1) >= 0)
		printf("  BEOS:APP_SIG: %s\n", signature.c_str());
	else
		fprintf(stderr, "  failed to set signature\n");

	if (node.WriteAttr("SYS:NAME", B_STRING_TYPE, 0, catalogEntry.c_str(),
			catalogEntry.size() + 1) >= 0)
		printf("  SYS:NAME: %s\n", catalogEntry.c_str());

	return 0;
}
