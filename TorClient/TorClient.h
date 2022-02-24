#pragma once

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <chrono>
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
#include <list>
#include <nlohmann/json.hpp>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QtGui>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QscrollArea>
#include <set>
#include <thread>
#include <QtextEdit>

using nlohmann::json;
namespace filesys = boost::filesystem;

class TorClient;

class TorProcess : public QWidget {
    Q_OBJECT
public:
    const std::string torlink, save_path;
    std::string tor_info, status_str, magnet_link;

    TorProcess(const std::string torlink_, const std::string save_path_, lt::session *s_, TorClient *parent_ = nullptr);

    ~TorProcess();

    QHBoxLayout *getBody() const;

private:
    /// <summary>
    /// downloads torrent files
    /// </summary>
    void torDownload();
    /// <summary>
    /// extracts information about torrent from file and net.
    /// </summary>
    void getTorInfo();

protected:
    bool stop;
    TorClient *parent;
    QHBoxLayout *body = new QHBoxLayout;
    QLabel *lname = new QLabel, *lstatus = new QLabel;
    QPushButton *bdelete = new QPushButton("delete"), *bshow_info = new QPushButton("show info");
    lt::session *s;
public slots:
    /// <summary>
    /// sets obtained torrent infromation to edit text box
    /// </summary>
    void setTextInfo();
    /// <summary>
    /// wraps delete button clicked signal, sends sendDeleteClicked(TorProcess *tp).
    /// </summary>
    void onDeleteClicked();

signals:
    /// <summary>
    /// sends signal with object link to be deleted
    /// </summary>
    void sendDeleteClicked(TorProcess *tp);

};


class TorClient : public QMainWindow {
    Q_OBJECT
protected:
    QWidget *wid = new QWidget;
    //QToolBar *tool_bar = new QToolBar;
    QVBoxLayout *main_ver = new QVBoxLayout, *process_block = new QVBoxLayout;
    QHBoxLayout *menue_hor = new QHBoxLayout;
    QMenuBar *menu_bar = this->menuBar();
    QMenu *mfile = new QMenu("File"), *moptions = new QMenu("Options"), *mview = new QMenu("View");
    QLabel *lsave_path = new QLabel;//, *name = new QLabel, *name = new QLabel;;
    QScrollArea *scroll_area = new QScrollArea;
    

    lt::session s;
    std::string j_data_path, save_path, tor_files_path;
    json j_data;
    std::list<TorProcess *> process_lines;
public:
    QTextEdit *bottom_text = new QTextEdit;

    TorClient(QWidget *parent = Q_NULLPTR);

private:
    /// <summary>
    /// loads all program saved data
    /// </summary>
    void loadData();
    /// <summary>
    /// adds torrent to data and remambers path to it.
    /// </summary>
    /// <param name="torlink">link to torrent file</param>
    /// <param name="cur_save_path">current save path</param>
    void addTorrent(const std::string &torlink, std::string cur_save_path = "");
    
public slots:
    /// <summary>
    /// gets torrent file location and downloads the content.
    /// </summary>
    void onOpenTorrent();
    /// <summary>
    /// deletes torrent session process and info about it.
    /// </summary>
    /// <param name="tproc"></param>
    void deleteTorrent(TorProcess *tproc);
    /// <summary>
    /// toggles debug console
    /// </summary>
    void toggleConsole();
    /// <summary>
    /// changes current save path
    /// </summary>
    void onChangeSavePath();
    /// <summary>
    /// displays current json form stored info
    /// </summary>
    void showData();
signals:
    /// <summary>
    /// sends signal
    /// </summary>
    void sendClosed();

};
