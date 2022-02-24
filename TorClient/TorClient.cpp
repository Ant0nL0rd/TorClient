#include "TorClient.h"
#include "libtorrent/bdecode.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/span.hpp"
#include "libtorrent/torrent_info.hpp"
#include "QWidget"
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <chrono>
#include <cinttypes> // for PRId64 et.al.
#include <csignal>
#include <cstdio> // for snprintf
#include <fstream>
#include <iostream>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <QDir>
#include <QFileDialog>
#include <QtGui>
#include <string>
#include <thread>

using nlohmann::json;
namespace filesys = boost::filesystem;

void clearLayout(QLayout *layout, bool deleteWidgets = true) {
	while (QLayoutItem *item = layout->takeAt(0)) {
		if (deleteWidgets) {
			if (QWidget *widget = item->widget())
				widget->deleteLater();
		}
		if (QLayout *childLayout = item->layout())
			clearLayout(childLayout, deleteWidgets);
		delete item;
	}
}

std::mutex file_lock, process_lock;

TorProcess::TorProcess(const std::string torlink_, const std::string save_path_, lt::session *s_, TorClient *parent_) : QWidget(parent_),
parent(parent_), torlink(torlink_), save_path(save_path_), s(s_), stop(false) {
	body->addWidget(lname);
	body->addWidget(lstatus);
	lstatus->setFixedWidth(500);
	filesys::path fpath(torlink);
	lname->setText(fpath.filename().string().c_str());
	connect(bshow_info, SIGNAL(clicked()), this, SLOT(setTextInfo()));
	getTorInfo();
	std::thread(&TorProcess::torDownload, this).detach();
	bdelete->setFixedWidth(100);
	bdelete->setFixedHeight(40);
	bshow_info->setFixedWidth(100);
	bshow_info->setFixedHeight(40);
	body->addWidget(bdelete);
	body->addWidget(bshow_info);
	connect(bdelete, SIGNAL(clicked()), this, SLOT(onDeleteClicked()));
	connect(this, SIGNAL(sendDeleteClicked(TorProcess *)), parent, SLOT(deleteTorrent(TorProcess *)));
	show();
}

TorProcess::~TorProcess() {
	stop = true;
	//delete body;
	//delete lname;
}

QHBoxLayout *TorProcess::getBody() const {
	return body;
}

using clk = std::chrono::steady_clock;

namespace {
	using clk = std::chrono::steady_clock;

	// return the name of a torrent status enum
	char const *state(lt::torrent_status::state_t s)
	{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#endif
		switch (s) {
		case lt::torrent_status::checking_files: return "checking";
		case lt::torrent_status::downloading_metadata: return "dl metadata";
		case lt::torrent_status::downloading: return "downloading";
		case lt::torrent_status::finished: return "finished";
		case lt::torrent_status::seeding: return "seeding";
		case lt::torrent_status::checking_resume_data: return "checking resume";
		default: return "<>";
		}
#ifdef __clang__
#pragma clang diagnostic pop
#endif
	}

	std::vector<char> load_file(char const *filename)
	{
		std::ifstream ifs(filename, std::ios_base::binary);
		ifs.unsetf(std::ios_base::skipws);
		return { std::istream_iterator<char>(ifs), std::istream_iterator<char>() };
	}

	// set when we're exiting
	std::atomic<bool> shut_down{ false };

	void sighandler(int) { shut_down = true; }

} // anonymous namespace

void TorProcess::torDownload() try {
	// load session parameters
	std::string sesssion_path = std::string("sessions/") + filesys::path(torlink).filename().string() + std::string(".session");
	std::string resume_path = std::string("sessions/") + filesys::path(torlink).filename().string() + std::string(".resume_file");
	auto session_params = load_file(sesssion_path.c_str());
	lt::session_params params = session_params.empty()
		? lt::session_params() : lt::read_session_params(session_params);
	params.settings.set_int(lt::settings_pack::alert_mask
		, lt::alert_category::error
		| lt::alert_category::storage
		| lt::alert_category::status);

	lt::session ses(params);
	clk::time_point last_save_resume = clk::now();

	// load resume data from disk and pass it in as we add the magnet link
	auto buf = load_file(resume_path.c_str());

	//torlink
	lt::add_torrent_params magnet = lt::parse_magnet_uri(magnet_link);
	if (buf.size()) {
		lt::add_torrent_params atp = lt::read_resume_data(buf);
		if (atp.info_hashes == magnet.info_hashes) magnet = std::move(atp);
	}
	magnet.save_path = save_path; // save in current dir
	ses.async_add_torrent(std::move(magnet));

	// this is the handle we'll set once we get the notification of it being
	// added
	lt::torrent_handle h;
	std::signal(SIGINT, &sighandler);
	// set when we're exiting
	bool done = false;
	for (;;) {
		std::vector<lt::alert *> alerts;
		ses.pop_alerts(&alerts);

		if (shut_down) {
			shut_down = false;
			auto const handles = ses.get_torrents();
			if (handles.size() == 1) {
				handles[0].save_resume_data(lt::torrent_handle::save_info_dict);
				done = true;
			}
		}
		for (lt::alert const *a : alerts) {
			if (auto at = lt::alert_cast<lt::add_torrent_alert>(a)) {
				h = at->handle;
			}
			// if we receive the finished alert or an error, we're done
			if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
				h.save_resume_data(lt::torrent_handle::save_info_dict);
				done = true;
			}
			if (lt::alert_cast<lt::torrent_error_alert>(a)) {
				std::cout << a->message() << std::endl;
				done = true;
				h.save_resume_data(lt::torrent_handle::save_info_dict);
			}
			// when resume data is ready, save it
			if (auto rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
				std::ofstream of(resume_path.c_str(), std::ios_base::binary);
				of.unsetf(std::ios_base::skipws);
				auto const b = write_resume_data_buf(rd->params);
				of.write(b.data(), int(b.size()));
				if (done) goto done;
			}
			if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
				if (done) goto done;
			}
			if (auto st = lt::alert_cast<lt::state_update_alert>(a)) {
				if (st->status.empty()) continue;
				// we only have a single torrent, so we know which one
				// the status is for
				lt::torrent_status const &s = st->status[0];
				std::cout << '\r' << state(s.state) << ' '
					<< (s.download_payload_rate / 1000) << " kB/s "
					<< (s.total_done / 1000) << " kB ("
					<< (s.progress_ppm / 10000) << "%) downloaded ("
					<< s.num_peers << " peers)\x1b[K" << std::endl;
				status_str = state(s.state) + ' ' + std::to_string(s.download_payload_rate / 1000)
					+ std::string(" kB/s ") + std::to_string(s.total_done / 1000) + std::string(" kB (") +
					std::to_string(s.progress_ppm / 10000) + "%) downloaded ("
					+ std::to_string(s.num_peers) + std::string(" peers)\x1b[K\n");
				lstatus->setText(status_str.c_str());
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		// ask the session to post a state_update_alert, to update our
		// state output for the torrent
		ses.post_torrent_updates();
		// save resume data once every 30 seconds
		if (clk::now() - last_save_resume > std::chrono::seconds(30)) {
			h.save_resume_data(lt::torrent_handle::save_info_dict);
			last_save_resume = clk::now();
		}
		if (stop) {
			break;
		}
	}
done:
	std::cout << "\nsaving session state" << std::endl;
	{
		std::ofstream of(sesssion_path.c_str(), std::ios_base::binary);
		of.unsetf(std::ios_base::skipws);
		auto const b = write_session_params_buf(ses.session_state()
			, lt::save_state_flags_t::all());
		of.write(b.data(), int(b.size()));
	}

	std::cout << "\ndone, shutting down" << std::endl;
}
catch (std::exception &e)
{
	std::cerr << "Error: " << e.what() << std::endl;
}

void TorProcess::getTorInfo() try {
	lt::load_torrent_limits cfg;
	bool show_pad = false;
	char const *filename = torlink.c_str();
	using namespace lt::literals;
	lt::torrent_info const t(filename, cfg);
	// print info about torrent
	if (!t.nodes().empty()) {
		tor_info += std::string("nodes:\n");
		for (auto const &i : t.nodes()) {
			char buff[500];
			std::snprintf(buff, 500, "%s: %d\n", i.first.c_str(), i.second);
			tor_info += std::string(std::move(buff));
		}
	}
	if (!t.trackers().empty()) {
		tor_info += std::string("trackers:\n");
		for (auto const &i : t.trackers()) {
			char buff[500];
			std::snprintf(buff, 500, "%2d: %s\n", i.tier, i.url.c_str());
			tor_info += std::string(std::move(buff));
		}
	}
	std::stringstream ih;
	ih << t.info_hashes().v1;
	if (t.info_hashes().has_v2()) {
		ih << ", " << t.info_hashes().v2;
	}
	char buff[500];
	magnet_link = make_magnet_uri(t);
	std::snprintf(buff, 500, 
		"number of pieces: %d\n"
		"piece length: %d\n"
		"info hash: %s\n"
		"comment: %s\n"
		"created by: %s\n"
		"magnet link: %s\n"
		"name: %s\n"
		"number of files: %d\n"
		"files:\n"
		, t.num_pieces()
		, t.piece_length()
		, ih.str().c_str()
		, t.comment().c_str()
		, t.creator().c_str()
		, make_magnet_uri(t).c_str()
		, t.name().c_str()
		, t.num_files());
	tor_info += std::string(std::move(buff));
	lt::file_storage const &st = t.files();
	for (auto const i : st.file_range()) {
		auto const first = st.map_file(i, 0, 0).piece;
		auto const last = st.map_file(i, std::max(std::int64_t(st.file_size(i)) - 1, std::int64_t(0)), 0).piece;
		auto const flags = st.file_flags(i);
		if ((flags & lt::file_storage::flag_pad_file) && !show_pad) continue;
		std::stringstream file_root;
		if (!st.root(i).is_all_zeros()) {
			file_root << st.root(i);
		}
		char buff[500];
		std::snprintf(buff, 500, 
			" %8" PRIx64 " %11" PRId64 " %c%c%c%c [ %5d, %5d ] %7u %s %s %s%s\n"
			, st.file_offset(i)
			, st.file_size(i)
			, ((flags & lt::file_storage::flag_pad_file) ? 'p' : '-')
			, ((flags & lt::file_storage::flag_executable) ? 'x' : '-')
			, ((flags & lt::file_storage::flag_hidden) ? 'h' : '-')
			, ((flags & lt::file_storage::flag_symlink) ? 'l' : '-')
			, static_cast<int>(first)
			, static_cast<int>(last)
			, std::uint32_t(st.mtime(i))
			, file_root.str().c_str()
			, st.file_path(i).c_str()
			, (flags & lt::file_storage::flag_symlink) ? "-> " : ""
			, (flags & lt::file_storage::flag_symlink) ? st.symlink(i).c_str() : "");
		tor_info += std::string(std::move(buff));
	}
	tor_info += "web seeds:\n";
	for (auto const &ws : t.web_seeds()) {
		char buff[500];
		std::snprintf(buff, 500,
			"%s %s\n"
			, ws.type == lt::web_seed_entry::url_seed ? "BEP19" : "BEP17"
			, ws.url.c_str());
		tor_info += std::string(std::move(buff));
	}
}
catch (std::exception const &e)
{
	std::cerr << "ERROR: " << e.what() << "\n";
}

void TorProcess::setTextInfo() {
	parent->bottom_text->setText(tor_info.c_str());
}

void TorProcess::onDeleteClicked() {
	emit sendDeleteClicked(this);
}

TorClient::TorClient(QWidget *parent) : QMainWindow(parent), j_data_path("data.json"), tor_files_path("tor_files/"), s() {
	this->setStyleSheet("border: 1px solid black;");
	setWindowTitle("TorClient");
	QAction *view_console_action = new QAction("toggle console"), *add_torrent_action = new QAction("Open"),
		*save_path_action = new QAction("change save path"), *view_json_action = new QAction("view data");
	mview->addAction(view_console_action);
	connect(view_console_action, SIGNAL(triggered()), this, SLOT(toggleConsole()));
	mview->addAction(view_json_action);
	connect(view_json_action, SIGNAL(triggered()), this, SLOT(showData()));
	mfile->addAction(add_torrent_action);
	connect(add_torrent_action, SIGNAL(triggered()), this, SLOT(onOpenTorrent()));
	moptions->addAction(save_path_action);
	connect(save_path_action, SIGNAL(triggered()), this, SLOT(onChangeSavePath()));

	
	menu_bar->addMenu(new QMenu("------------------>"));
	menu_bar->addMenu(mfile); //menue bar declaration
	menu_bar->addMenu(moptions);
	menu_bar->addMenu(mview);
	menu_bar->update();
	
	scroll_area->setBackgroundRole(QPalette::Dark);
	scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll_area->setLayout(process_block);
	main_ver->addItem(menue_hor);
	main_ver->addWidget(scroll_area); 
	main_ver->addWidget(bottom_text);
	menue_hor->addWidget(lsave_path);

	wid->setLayout(main_ver);
	wid->setMinimumSize(1200, 600);
	this->setCentralWidget(wid);
	loadData();



	BOOST_LOG_TRIVIAL(trace) << "TORCLIENT INITIALISATION FINIHSED";
	show();
}

void TorClient::onOpenTorrent() {
	std::string str = QFileDialog::getOpenFileName(this, "Open a torrent file", QDir::currentPath(), "*.torrent").toStdString();
	if (str.size() > 0) {
		addTorrent(str);
	}
}

void TorClient::onChangeSavePath() {
	QFileDialog dialog(this, "Choose new save path", QDir::currentPath());
	dialog.setFileMode(QFileDialog::Directory);
	dialog.exec();
	save_path = dialog.selectedFiles()[0].toStdString();
	j_data["save_path"] = save_path;
	filesys::ofstream data_file(j_data_path);
	data_file << j_data.dump();
	lsave_path->setText((std::string("save path:") + save_path).c_str());
	BOOST_LOG_TRIVIAL(trace) << "SAVE PATH UPDATED";
}

void TorClient::addTorrent(const std::string& torlink, std::string cur_save_path) {
	if (cur_save_path.empty()) {
		cur_save_path = save_path;
	}
	if(!filesys::exists(tor_files_path)) {
		filesys::create_directories(tor_files_path);
	}
	std::string new_path = std::string(tor_files_path) + filesys::path(torlink).filename().string();
	if (!filesys::exists(new_path)) {
		filesys::copy(torlink, new_path);
	}
	j_data["torlinks"][new_path] = cur_save_path;
	filesys::ofstream data_file(j_data_path);
	data_file << j_data.dump();
	TorProcess *new_process = new TorProcess(new_path, cur_save_path, &s, this);
	process_lines.push_back(new_process);
	process_block->addLayout(new_process->getBody());
	show();
	BOOST_LOG_TRIVIAL(trace) << "TORRENT ADDED";
}

void TorClient::deleteTorrent(TorProcess *tproc) {
	j_data["torlinks"].erase(tproc->torlink);
	filesys::ofstream data_file(j_data_path);
	data_file << j_data.dump();
	process_block->removeItem(tproc->getBody());
	process_lines.remove(tproc);
	clearLayout(tproc->getBody());
	delete tproc;
	process_block->update();
	

	BOOST_LOG_TRIVIAL(trace) << "TORRENT DELETED";
}

void TorClient::loadData() {
	if (filesys::exists(j_data_path) && filesys::is_regular_file(j_data_path)) {
		filesys::ifstream data_file(j_data_path);
		j_data << data_file;
	}
	if (!filesys::exists("sessions")) {
		boost::filesystem::create_directory("sessions");
	}
	else {
		j_data["save_path"] = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation).toStdString() + "/saved_torrents";
		filesys::ofstream data_file(j_data_path);
		data_file << j_data.dump();
	}
	for(const auto &val : j_data["torlinks"].items()) {
		addTorrent(val.key(), val.value());
	}
	save_path = j_data["save_path"];
	lsave_path->setText((std::string("save path:") + save_path).c_str());
	BOOST_LOG_TRIVIAL(trace) << "TORRENT LIST LOADED";
}

void TorClient::toggleConsole() {
	
	if (::IsWindowVisible(::GetConsoleWindow())) {
		::ShowWindow(::GetConsoleWindow(), SW_HIDE);
		BOOST_LOG_TRIVIAL(trace) << "CONSOLE HIDDEN";
	}
	else {
		::ShowWindow(::GetConsoleWindow(), SW_SHOW);
		BOOST_LOG_TRIVIAL(trace) << "CONSOLE SHOWN";
	}
	BOOST_LOG_TRIVIAL(trace) << "CONSOLE TOGGLED";
}

void TorClient::showData() {
	bottom_text->setText(j_data.dump().c_str());
}