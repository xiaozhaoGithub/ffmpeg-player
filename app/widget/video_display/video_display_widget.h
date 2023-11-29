#ifndef VIDEO_DISPLAY_WIDGET_H_
#define VIDEO_DISPLAY_WIDGET_H_

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "render/opengl/video_surface_gl.h"
#include "render/qpainter/video_player_widget.h"
#include "widget/common/widgets.h"

class VideoDisplayWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoDisplayWidget(QWidget* parent = nullptr);
    ~VideoDisplayWidget();

private slots:
    void SelectMediaClicked();
    void PlayClicked();
    void PauseClicked();
    void StopClicked();
    void RecordClicked();
    void StopRecordClicked();
    void DecodeBtnClicked(int id);

    void PlayState(bool playing);
    void RecordState(bool recording);

private:
    // VideoPlayerWidget* player_;
    VideoSurfaceGL* player_;

    FolderLineEdit* file_edit_;
    QPushButton* play_btn_;
    QPushButton* pause_btn_;
    QStackedWidget* play_stop_widget_;

    QPushButton* record_btn_;
    QPushButton* stop_record_btn_;
    QStackedWidget* record_widget_;

    MediaInfo media_;
};

#endif
