#include "PresetUpdater.hpp"

#include <thread>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <wx/app.h>
#include <wx/event.h>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/Semver.hpp"

namespace fs = boost::filesystem;


namespace Slic3r {


// TODO: proper URL
// TODO: Actually, use index
static const std::string SLIC3R_VERSION_URL = "https://gist.githubusercontent.com/vojtechkral/4d8fd4a3b8699a01ec892c264178461c/raw/e9187c3e15ceaf1a90f29b7c43cf3ccc746140f0/slic3rPE.version";
enum {
	SLIC3R_VERSION_BODY_MAX = 256,
};

struct PresetUpdater::priv
{
	int version_online_event;
	bool version_check;
	bool preset_update;

	fs::path cache_path;
	bool cancel;
	std::thread thread;

	priv(int event);

	void download(const std::set<VendorProfile> vendors) const;
};

PresetUpdater::priv::priv(int event) :
	version_online_event(event),
	version_check(false),
	preset_update(false),
	cache_path(fs::path(Slic3r::data_dir()) / "cache"),
	cancel(false)
{}

void PresetUpdater::priv::download(const std::set<VendorProfile> vendors) const
{
	if (!version_check) { return; }

	// Download current Slic3r version
	Http::get(SLIC3R_VERSION_URL)
		.size_limit(SLIC3R_VERSION_BODY_MAX)
		.on_progress([this](Http::Progress, bool &cancel) {
			cancel = this->cancel;
		})
		.on_complete([&](std::string body, unsigned http_status) {
			boost::trim(body);
			wxCommandEvent* evt = new wxCommandEvent(version_online_event);
			evt->SetString(body);
			GUI::get_app()->QueueEvent(evt);
		})
		.perform_sync();

	if (!preset_update) { return; }

	// Donwload vendor preset bundles
	for (const auto &vendor : vendors) {
		if (cancel) { return; }

		// TODO: Proper caching

		auto target_path = cache_path / vendor.id;
		target_path += ".ini";

		Http::get(vendor.config_update_url)
			.on_progress([this](Http::Progress, bool &cancel) {
				cancel = this->cancel;
			})
			.on_complete([&](std::string body, unsigned http_status) {
				fs::fstream file(target_path, std::ios::out | std::ios::binary | std::ios::trunc);
				file.write(body.c_str(), body.size());
			})
			.perform_sync();
	}
}

PresetUpdater::PresetUpdater(int version_online_event, AppConfig *app_config) :
	p(new priv(version_online_event))
{
	p->preset_update = app_config->get("preset_update") == "1";
	// preset_update implies version_check:
	p->version_check = p->preset_update || app_config->get("version_check") == "1";
}


// Public

PresetUpdater::~PresetUpdater()
{
	if (p && p->thread.joinable()) {
		p->cancel = true;
		p->thread.join();
	}
}

void PresetUpdater::download(PresetBundle *preset_bundle)
{

	// Copy the whole vendors data for use in the background thread
	// Unfortunatelly as of C++11, it needs to be copied again
	// into the closure (but perhaps the compiler can elide this).
	std::set<VendorProfile> vendors = preset_bundle->vendors;

	p->thread = std::move(std::thread([this, vendors]() {
		this->p->download(std::move(vendors));
	}));
}

void PresetUpdater::init_vendors()
{
	const auto vendors_rources = fs::path(resources_dir()) / "profiles";
	const auto vendors_data = fs::path(Slic3r::data_dir()) / "vendor";

	for (fs::directory_iterator it(vendors_rources); it != fs::directory_iterator(); ++it) {
		if (it->path().extension() == ".ini") {
			auto vp = VendorProfile::from_ini(it->path(), false);
			const auto path_in_data = vendors_data / it->path().filename();

			if (! fs::exists(path_in_data) || VendorProfile::from_ini(path_in_data, false).config_version < vp.config_version) {
				// FIXME: update vendor bundle properly when snapshotting is ready
				PresetBundle::install_vendor_configbundle(it->path());
			}
		}
	}
}


}