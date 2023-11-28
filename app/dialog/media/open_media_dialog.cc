#include "open_media_dialog.h"

#include <QCameraInfo>
#include <QFileDialog>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/def.h"

OpenMediaDialog::OpenMediaDialog(QWidget* parent)
    : ConfirmDialog(parent)
{
    setWindowTitle(tr("Open Media"));

    tabwidget_ = new QTabWidget(this);

    file_edit_ = new QLineEdit(this);
    file_edit_->setFixedWidth(360);
    auto select_file_btn = new QPushButton(tr("..."), this);
    connect(select_file_btn, &QPushButton::clicked, this, &OpenMediaDialog::SelectFileClicked);

    url_edit_ = new QLineEdit(this);
    url_edit_->setFixedWidth(360);
    url_edit_->setText("https://media.w3.org/2010/05/sintel/trailer.mp4");

    capture_dev_combo_ = new QComboBox(this);
    capture_dev_combo_->setFixedWidth(360);
    for (const auto& camera : QCameraInfo::availableCameras()) {
#ifdef Q_OS_WIN
        capture_dev_combo_->addItem(camera.description());
#elif defined(Q_OS_LINUX)
        capture_dev_combo_->addItem(camera.deviceName());
#endif
    }

    // file
    auto file_layout = new QHBoxLayout;
    file_layout->addWidget(file_edit_);
    file_layout->addWidget(select_file_btn);
    file_layout->setAlignment(Qt::AlignCenter);

    auto file_widget = new QWidget(this);
    auto file_widget_layout = new QVBoxLayout(file_widget);
    file_widget_layout->addLayout(file_layout);
    file_widget_layout->setAlignment(Qt::AlignCenter);

    // network
    auto network_widget = new QWidget(this);
    auto network_widget_layout = new QVBoxLayout(network_widget);
    network_widget_layout->addWidget(url_edit_);
    network_widget_layout->setAlignment(Qt::AlignCenter);

    // capture
    auto capture_widget = new QWidget(this);
    auto capture_widget_layout = new QVBoxLayout(capture_widget);
    capture_widget_layout->addWidget(capture_dev_combo_);
    capture_widget_layout->setAlignment(Qt::AlignCenter);

    tabwidget_->insertTab(kFile, file_widget, tr("File Source"));
    tabwidget_->insertTab(kNetwork, network_widget, tr("Network Source"));
    tabwidget_->insertTab(kCapture, capture_widget, tr("Capture Source"));

    auto main_layout = new QVBoxLayout(main_widget_);
    main_layout->addWidget(tabwidget_);
}

OpenMediaDialog::~OpenMediaDialog() {}

MediaInfo OpenMediaDialog::media()
{
    MediaInfo media;

    int index = tabwidget_->currentIndex();
    media.type = (MediaSourceType)index;

    if (index == kFile) {
        media.src = file_edit_->text().toStdString();
    } else if (index == kNetwork) {
        media.src = url_edit_->text().toStdString();
    } else {
        media.src = capture_dev_combo_->currentText().toStdString();
    }

    return media;
}

void OpenMediaDialog::SelectFileClicked()
{
    QFileDialog dlg;
    int ret = dlg.exec();
    if (ret == QDialog::Accepted) {
        auto files = dlg.selectedFiles();
        file_edit_->setText(files.at(0));
        file_edit_->setToolTip(files.at(0));
    }
}
