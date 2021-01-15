#include "ViewerWidget.h"
#include <QTimer>

#include "VmbImageTransformHelper.hpp"
#include "ExternLib/qcustomplot/qcustomplot.h"
#include <tuple>
#include <QDebug>

using AVT::VmbAPI::Frame;
using AVT::VmbAPI::FramePtr;

ViewerWidget::ViewerWidget(QWidget* parent, Qt::WindowFlags flag,
    QString sID, 
    bool bAutoAdjustPacketSize, CameraPtr pCam)
    : QWidget(parent, flag)
    , m_Controller(NULL)
    , m_InformationWindow(NULL)
    , m_bHasJustStarted(false)
    , m_bIsFirstStart(true)
    , m_bIsCameraRunning(false)
    , m_bIsCamOpen(false)
    , m_bIsRedHighlighted(false)
    , m_bIsViewerWindowClosing(false)
    , m_bIsDisplayEveryFrame(false)
    //, m_ImageOptionDialog(NULL)
    //, m_saveFileDialog(NULL)
    //, m_getDirDialog(NULL)
    //, m_bIsTriggeredByMultiSaveBtn(false)
    //, m_nNumberOfFramesToSave(0)
    , m_FrameBufferCount(BUFFER_COUNT)
    , m_pCam(pCam)
{
    VmbError_t errorType;
    QTime openTimer;
    openTimer.start();

    /* setup information window */
    m_DiagInfomation = new QDialog(this);
    m_DiagInfomation->setModal(false);
    m_DiagInfomation->setWindowTitle("Information/Logger for " + sID);
    m_DiagInfomation->setStyleSheet("background-color: rgb(255, 255, 255); font: 9pt;");//font: 10pt;
    m_InformationWindow = new MainInformationWindow(0, 0, m_pCam);
    m_InformationWindow->openLoggingWindow();
    QVBoxLayout* infoLayout = new QVBoxLayout(m_DiagInfomation);
    infoLayout->addWidget(m_InformationWindow);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    //m_DiagInfomation->show();


    /*connect to camera*/
    errorType = m_pCam->Open(VmbAccessModeFull);
    m_sAccessMode = tr("(FULL ACCESS)"); //it is always full access since we do not allow other connection type
    bAutoAdjustPacketSize = false;
    m_OpenError = errorType;
    
    if (VmbErrorSuccess != errorType)
    {
        openTimer.elapsed();
        return;
    }

    m_sCameraID = sID;
    if (!m_sAccessMode.isEmpty())
    {
        sID.append(" ");
        sID.append(m_sAccessMode);
    }
    this->setWindowTitle(sID);
    m_bIsCamOpen = true;


    /*QCP viewer widget: colormap + side/bottom plot*/
    m_QCP = QSharedPointer<QCustomPlot>(new QCustomPlot());
    m_QCP->plotLayout()->clear();
    m_QCPcenterAxisRect = new QCPAxisRect(m_QCP.data());
    m_QCPbottomAxisRect = new QCPAxisRect(m_QCP.data());
    m_QCPleftAxisRect = new QCPAxisRect(m_QCP.data());
    m_QCP->plotLayout()->addElement(0, 1, m_QCPcenterAxisRect);
    m_QCP->plotLayout()->addElement(0, 0, m_QCPleftAxisRect);
    m_QCP->plotLayout()->addElement(1, 1, m_QCPbottomAxisRect);

    m_QCPcenterAxisRect->setupFullAxesBox(true);
    for (auto& plt : { m_QCPleftAxisRect, m_QCPbottomAxisRect })
    { plt->setupFullAxesBox(false); }
    m_QCPleftAxisRect->axis(QCPAxis::atRight)->setTickLabels(true);
    m_QCPbottomAxisRect->axis(QCPAxis::atTop)->setTickLabels(true);

    QCPMarginGroup* marginGroup = new QCPMarginGroup(m_QCP.data());
    m_QCPbottomAxisRect->setMarginGroup(QCP::msLeft | QCP::msRight, marginGroup);
    m_QCPleftAxisRect->setMarginGroup(QCP::msTop | QCP::msBottom, marginGroup);
    m_QCPcenterAxisRect->setMarginGroup(QCP::msAll, marginGroup);


    m_colorScale = new QCPColorScale(m_QCP.data());
    m_colorScale->setType(QCPAxis::atRight);
    m_colorScale->setRangeZoom(false);
    m_colorScale->setRangeDrag(false);
    m_QCP->plotLayout()->addElement(0, 2, m_colorScale);
    m_colorScale->setMarginGroup(QCP::msBottom | QCP::msTop, marginGroup);

    m_QCPbottomAxisRect->setMinimumSize(600, 120);
    m_QCPbottomAxisRect->setMaximumSize(600, 120);
    m_QCPleftAxisRect->setMinimumSize(120, 600);
    m_QCPleftAxisRect->setMaximumSize(120, 600);

    // move newly created axes on "axes" layer and grids on "grid" layer:
    foreach(QCPAxisRect * rect, m_QCP->axisRects())
    {
        foreach(QCPAxis * axis, rect->axes())
        {
            axis->setLayer("axes");
            axis->grid()->setLayer("grid");
        }
    }

    /*m_QCP->setNotAntialiasedElements(QCP::aeAll);*/
    
    m_colorMap = QSharedPointer<QCPColorMap>(new QCPColorMap(
        m_QCPcenterAxisRect->axis(QCPAxis::atBottom), 
        m_QCPcenterAxisRect->axis(QCPAxis::atLeft)));
    m_colorMap->setInterpolate(false);
    m_colorMap->setColorScale(m_colorScale);
    m_colorMap->setGradient(QCPColorGradient::gpGrayscale);
    
    //m_colorMap->valueAxis()->setTickLabelPadding(0);

    m_bottomGraph = QSharedPointer<QCPGraph>(
        new QCPGraph(m_QCPbottomAxisRect->axis(QCPAxis::atTop),
        m_QCPbottomAxisRect->axis(QCPAxis::atLeft)));
    m_leftGraph = QSharedPointer<QCPGraph>(
        new QCPGraph(m_QCPleftAxisRect->axis(QCPAxis::atRight),
            m_QCPleftAxisRect->axis(QCPAxis::atBottom)));
    m_leftGraph->valueAxis()->setTickLabelRotation(90);
    
    m_QCP->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom); // this will also allow rescaling the color scale by dragging/zooming
    m_QCPbottomAxisRect->setRangeDragAxes(m_bottomGraph->keyAxis(), m_bottomGraph->valueAxis());
    m_QCPbottomAxisRect->setRangeZoomAxes(m_bottomGraph->keyAxis(), m_bottomGraph->valueAxis());
    m_QCPleftAxisRect->setRangeDragAxes(m_leftGraph->valueAxis(), m_leftGraph->keyAxis());
    m_QCPleftAxisRect->setRangeZoomAxes(m_leftGraph->valueAxis(), m_leftGraph->keyAxis());
    for (auto& p : { m_QCPbottomAxisRect ,m_QCPleftAxisRect ,m_QCPcenterAxisRect})
    { p->setRangeZoomFactor(0.95); }

    // setup a ticker for colormap that only gives integer ticks:
    QSharedPointer<QCPAxisTickerFixed> intTicker(new QCPAxisTickerFixed);
    intTicker->setTickStep(1.0);
    intTicker->setScaleStrategy(QCPAxisTickerFixed::ssMultiples);
    m_colorMap->keyAxis()->setTicker(intTicker);
    m_colorMap->valueAxis()->setTicker(intTicker);
    m_leftGraph->keyAxis()->setTicker(intTicker);
    m_bottomGraph->keyAxis()->setTicker(intTicker);

    /*tracer for the bottom and left plot*/
    m_QCPtracerbottom = new QCPItemTracer(m_QCP.data());
    m_QCPtracerbottom->setClipAxisRect(m_bottomGraph->keyAxis()->axisRect());
    m_QCPtracerbottom->setGraph(m_bottomGraph.data());
    m_QCPtracerleft = new QCPItemTracer(m_QCP.data());
    m_QCPtracerleft->setClipAxisRect(m_leftGraph->keyAxis()->axisRect());
    m_QCPtracerleft->setGraph(m_leftGraph.data());
    for (auto& tracer : { m_QCPtracerbottom ,m_QCPtracerleft })
    {
        tracer->setInterpolating(false);
        tracer->setStyle(QCPItemTracer::tsCircle);
        tracer->setPen(QPen(Qt::red));
        tracer->setBrush(Qt::red);
        tracer->setSize(7);
    }

    m_QCPtraceTextbottom = new QCPItemText(m_QCP.data());
    m_QCPtraceTextbottom->position->setParentAnchor(m_QCPtracerbottom->position);
    m_QCPtraceTextbottom->setClipAxisRect(m_QCPbottomAxisRect);
    m_QCPtraceTextbottom->position->setCoords(0, 12);
    m_QCPtraceTextleft = new QCPItemText(m_QCP.data());
    m_QCPtraceTextleft->position->setParentAnchor(m_QCPtracerleft->position);
    m_QCPtraceTextleft->setClipAxisRect(m_QCPleftAxisRect);
    m_QCPtraceTextleft->setRotation(90);
    m_QCPtraceTextleft->position->setCoords(-12, 0);

    connect(m_QCP.data(), &QCustomPlot::mouseMove, this, &ViewerWidget::onSetMousePosInCMap);
    connect(m_QCP.data(), &QCustomPlot::mousePress, this, [this](QMouseEvent* event) {
        if (event->button() == Qt::LeftButton) { return false; }
        else { m_ContextMenu->exec(QCursor::pos()); return true; } });
    connect(m_QCP.data(), &QCustomPlot::mouseDoubleClick, this, [this]() {
        m_pImgCThread->setDefaultView();
        m_QCP->replot(); });
    


    /* add Viewer Widget to ViewerWindow*/
    //m_pScene = QSharedPointer<QGraphicsScene>(new QGraphicsScene());
    //m_PixmapItem = new QGraphicsPixmapItem();
    //m_ScreenViewer = new GraphViewer(this);
    //m_ScreenViewer->setAlignment(Qt::AlignCenter);
    //m_ScreenViewer->setScene(m_pScene.data());
    //m_pScene->addItem(m_PixmapItem);
    //m_ScreenViewer->show();
    //m_ScreenViewer->setStyleSheet("background-color: rgb(196,0, 0); color: rgb(255,255,255)");


    /*set image layout*/
    QVBoxLayout* m_VertLayout = new QVBoxLayout(this);
    QLabel* namelabel = new QLabel(sID);
    namelabel->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    m_VertLayout->addWidget(namelabel, 0);
    //m_VertLayout->addWidget(m_ScreenViewer, 1);
    m_VertLayout->addWidget(m_QCP.data(), 1);
    //this->setStyleSheet("background-color: rgb(85, 100, 100)");
    //this->setWindowFlags(Qt::Widget);


    /* add DiagController Controller */
    m_DiagController = new QDialog(this);
    m_DiagController->setModal(false);
    m_DiagController->setWindowTitle("Controller for " + sID);

    /* add Controller Tree */
    QWidget* widgetTree = new QWidget();
    m_Description = new QTextEdit();
    m_Controller = new ControllerTreeWindow(m_sCameraID, widgetTree, bAutoAdjustPacketSize, m_pCam);
    if (VmbErrorSuccess != m_Controller->getTreeStatus())
    {
        onFeedLogger("ERROR: ControllerTree returned: " + QString::number(m_Controller->getTreeStatus()) + " " + Helper::mapReturnCodeToString(m_Controller->getTreeStatus()));
    }

    m_Description->setLineWrapMode(QTextEdit::NoWrap);
    m_Description->setReadOnly(true);
    m_Description->setStyleSheet("font: 12px;\n" "font-family: Verdana;\n");

    /* add Filter Pattern */
    QHBoxLayout* pattern_HLayout = new QHBoxLayout();
    m_FilterPatternLineEdit = new LineEditCompleter(this);
    m_FilterPatternLineEdit->setText(tr("Example: Gain|Width"));
    m_FilterPatternLineEdit->setToolTip(tr("To filter multiple features, e.g: Width|Gain|xyz|etc"));
    m_FilterPatternLineEdit->setCompleter(m_Controller->getListCompleter());
    m_FilterPatternLineEdit->setMinimumWidth(200);
    QLabel* filterPatternLabel = new QLabel(tr("Filter pattern:"));
    filterPatternLabel->setStyleSheet("font-weight: bold;");
    QPushButton* patternButton = new QPushButton(tr("Search"));

    /*put controller related into controller dialog*/
    pattern_HLayout->addWidget(filterPatternLabel);
    pattern_HLayout->addWidget(m_FilterPatternLineEdit);
    pattern_HLayout->addWidget(patternButton);

    QWidget* ctrlTreeVerticalLayoutWidget = new QWidget();
    QVBoxLayout* ctrlTreeVerticalLayout = new QVBoxLayout();
    ctrlTreeVerticalLayout->addLayout(pattern_HLayout);
    ctrlTreeVerticalLayout->addWidget(m_Controller);
    ctrlTreeVerticalLayout->setContentsMargins(0, 0, 0, 0);
    ctrlTreeVerticalLayoutWidget->setLayout(ctrlTreeVerticalLayout);

    QSplitter* splitter = new QSplitter(m_DiagController);
    QList<int> listSize;
    listSize << 5000;
    splitter->setChildrenCollapsible(false);
    splitter->setOrientation(Qt::Vertical);
    splitter->addWidget(ctrlTreeVerticalLayoutWidget);
    splitter->addWidget(m_Description);
    splitter->setSizes(listSize);

    QVBoxLayout* ctrlDiaVLayout = new QVBoxLayout();
    ctrlDiaVLayout->addWidget(splitter);
    m_DiagController->setLayout(ctrlDiaVLayout);
    
    connect(m_FilterPatternLineEdit, SIGNAL(returnPressed()), this, SLOT(textFilterChanged()));
    connect(patternButton, SIGNAL(clicked(bool)), this, SLOT(textFilterChanged()));
    
    connect(m_Controller, SIGNAL(setDescription(const QString&)), this, SLOT(onSetDescription(const QString&)));
    connect(m_Controller, SIGNAL(setEventMessage(const QStringList&)), this, SLOT(onSetEventMessage(const QStringList&)), Qt::QueuedConnection);
    connect(m_Controller, SIGNAL(acquisitionStartStop(const QString&)), this, SLOT(onAcquisitionStartStop(const QString&))); // this eventually calls RegisterObserver in Frame.h which make sure  As new frames arrive, the observer's FrameReceived method will be called.  Only one observer can be registered.
    connect(m_Controller, SIGNAL(updateBufferSize()), this, SLOT(onPrepareCapture()));
    connect(m_Controller, SIGNAL(resetFPS()), this, SLOT(onResetFPS()));
    connect(m_Controller, SIGNAL(logging(const QString&)), this, SLOT(onFeedLogger(const QString&)));


    /*create context menu*/
    m_ContextMenu = new QMenu;
    this->setContextMenuPolicy(Qt::CustomContextMenu);
        
    m_aStartStopCap = new QAction("&Streaming");
    m_ContextMenu->addAction(m_aStartStopCap);
    m_aStartStopCap->setCheckable(true);
    m_aStartStopCap->setEnabled(isStreamingAvailable());
    connect(m_aStartStopCap, &QAction::triggered, this, &ViewerWidget::on_ActionFreerun_triggered);
    
    m_aDiagCtrler = new QAction("Con&troller");
    m_ContextMenu->addAction(m_aDiagCtrler);
    connect(m_aDiagCtrler, &QAction::triggered, this, [&]() {m_DiagController->show(); });
    
    m_aDiagInfo = new QAction("Infor&mation");
    m_ContextMenu->addAction(m_aDiagInfo);
    connect(m_aDiagInfo, &QAction::triggered, this, [&]() {m_DiagInfomation->show(); });

    m_ContextMenu->addSeparator();

    m_aSetCurrScrROI = new QAction("SetCurrentRO&I");
    m_ContextMenu->addAction(m_aSetCurrScrROI);
    connect(m_aSetCurrScrROI, &QAction::triggered, this, &ViewerWidget::SetCurrentScreenROI);

    m_aResetFullROI = new QAction("ResetFullROI");
    m_ContextMenu->addAction(m_aResetFullROI);
    connect(m_aResetFullROI, &QAction::triggered, this, &ViewerWidget::ResetFullROI);


    m_aPlotTracer = new QAction("Tracer");
    m_aPlotTracer->setCheckable(true);
    m_aPlotTracer->setChecked(true);
    m_ContextMenu->addAction(m_aPlotTracer);
    connect(m_aPlotTracer, &QAction::triggered, this, [this]() {
        for (auto& tra : { m_QCPtracerbottom,m_QCPtracerleft })
        {
            m_aPlotTracer->isChecked() ? tra->setVisible(true) : tra->setVisible(false);
        }
        for (auto& tra : { m_QCPtraceTextbottom,m_QCPtraceTextleft })
        {
            m_aPlotTracer->isChecked() ? tra->setVisible(true) : tra->setVisible(false);
        }
        m_QCP->replot(); });
        

    m_ContextMenu->addSeparator();

    //connecting the following action to a slot happens at cameraMainWindow, be careful of the order
    m_aCamlist = new QAction("&Camera");
    m_ContextMenu->addAction(m_aCamlist);
    m_aDisconnect = new QAction("&Disconnect");
    m_ContextMenu->addAction(m_aDisconnect);

    connect(this, SIGNAL(customContextMenuRequested(const QPoint&)),
        this, SLOT(OnShowContextMenu(const QPoint&)));
    
    /* create FrameObserver to get frames from camera, add for QCPColorMap */
    SP_SET(m_pFrameObs, new FrameObserver(m_pCam));
    //connect(SP_ACCESS(m_pFrameObs), SIGNAL(frameReadyFromObserver(QImage, const QString&, const QString&, const QString&)),
    //    this, SLOT(onimageReady(QImage, const QString&, const QString&, const QString&)));
    //connect(SP_ACCESS(m_pFrameObs), SIGNAL(frameReadyFromObserver(QVector<ushort>, const QString&, const QString&, const QString&)),
    //    this, SLOT(onimageReady(QVector<ushort>, const QString&, const QString&, const QString&)));
    //connect(SP_ACCESS(m_pFrameObs), SIGNAL(frameReadyFromObserver(std::vector<ushort>, const QString&, const QString&, const QString&)),
    //    this, SLOT(onimageReady(std::vector<ushort>, const QString&, const QString&, const QString&)));
    
    connect(SP_ACCESS(m_pFrameObs), SIGNAL(frameReadyFromObserverFullBitDepth(tFrameInfo)),
        this, SLOT(onFullBitDepthImageReady(tFrameInfo)));
    connect(SP_ACCESS(m_pFrameObs), SIGNAL(setCurrentFPS(const QString&)),
        this, SLOT(onSetCurrentFPS(const QString&)));
    connect(SP_ACCESS(m_pFrameObs), SIGNAL(setFrameCounter(const unsigned int&)),
        this, SLOT(onSetFrameCounter(const unsigned int&)));

    connect(SP_ACCESS(m_pFrameObs)->ImageProcessThreadPtr().data(),
        &ImageProcessingThread::logging, this, &ViewerWidget::onFeedLogger);


    /*create image calculating thread*/
    m_pImgCThread = new ImageCalculatingThread(m_pFrameObs, m_pCam, m_QCP, m_colorMap, m_bottomGraph, m_leftGraph);
    connect(m_pImgCThread, &ImageCalculatingThread::imageReadyForPlot,
        this, &ViewerWidget::onimageReadyFromCalc);
    connect(this, &ViewerWidget::acquisitionRunning, this, &ViewerWidget::onImageCalcStartStop);
    connect(m_QCP.data(), &QCustomPlot::mouseMove, m_pImgCThread, &ImageCalculatingThread::updateMousePos);
    connect(m_pImgCThread, &ImageCalculatingThread::logging, this, &ViewerWidget::onFeedLogger);


    m_Timer = new QTimer(this);
    
    /* Statusbar */
    QStatusBar* statusbar1 = new QStatusBar;
    QStatusBar* statusbar2 = new QStatusBar;
    m_OperatingStatusLabel = new QLabel(" Ready ");
    m_FormatButton = new QPushButton;
    m_ImageSizeButtonH = new QPushButton;
    m_ImageSizeButtonW = new QPushButton;
    m_FramesLabel = new QLabel;
    m_FramerateButton = new QPushButton;
    m_CursorScenePosLabel = new QLabel;
    m_ExposureTimeButton = new QPushButton;
    m_CameraGainButton = new QPushButton();
    //for (auto& tmp : QList<QLabel*>{ m_OperatingStatusLabel ,m_FormatLabel ,m_ImageSizeLabel,m_CursorScenePosLabel })
    //{
    //    tmp->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    //}

    QWidget* imageSizeBtn = new QWidget();
    QHBoxLayout* imageSizeBtnLayout = new QHBoxLayout(imageSizeBtn);
    imageSizeBtnLayout->setMargin(0);
    imageSizeBtnLayout->addWidget(m_ImageSizeButtonH);
    imageSizeBtnLayout->addWidget(m_ImageSizeButtonW);
    statusbar1->addWidget(m_OperatingStatusLabel);
    statusbar1->addWidget(imageSizeBtn);
    statusbar1->addWidget(m_FormatButton);
    statusbar2->addWidget(m_ExposureTimeButton);
    statusbar2->addWidget(m_CameraGainButton);
    statusbar2->addWidget(m_FramesLabel);
    statusbar2->addWidget(m_FramerateButton);
    statusbar1->addWidget(m_CursorScenePosLabel);

    m_OperatingStatusLabel->setStyleSheet("background-color: rgb(0,0, 0); color: rgb(255,255,255)");
    for (auto& btn: { m_ImageSizeButtonH ,m_ImageSizeButtonW,m_CameraGainButton,m_ExposureTimeButton,m_FormatButton,m_FramerateButton })
        btn->setStyleSheet("border: none; color: rgb(128, 89, 255)");
    m_VertLayout->addWidget(statusbar1, 0);
    m_VertLayout->addWidget(statusbar2, 0);
    connect(m_FormatButton, &QPushButton::clicked, this, [this]() {
        m_Controller->updateRegisterFeature();
        QList<QStandardItem*> tmp = m_Controller->controllerModel()->findItems("PixelFormat", Qt::MatchRecursive | Qt::MatchWrap);
        if (!tmp.isEmpty()) { m_Controller->onClicked(tmp.at(0)->index().siblingAtColumn(1)); }
        });
    connect(m_ImageSizeButtonH, &QPushButton::clicked, this, [this]() {
        m_Controller->updateRegisterFeature();
        QList<QStandardItem*> tmp = m_Controller->controllerModel()->findItems("Height", Qt::MatchRecursive | Qt::MatchWrap);
        if (!tmp.isEmpty()) { m_Controller->onClicked(tmp.at(0)->index().siblingAtColumn(1)); }
        });
    connect(m_ImageSizeButtonW, &QPushButton::clicked, this, [this]() {
        m_Controller->updateRegisterFeature();
        QList<QStandardItem*> tmp = m_Controller->controllerModel()->findItems("Width", Qt::MatchRecursive | Qt::MatchWrap);
        if (!tmp.isEmpty()) { m_Controller->onClicked(tmp.at(0)->index().siblingAtColumn(1)); }
        });
    connect(m_CameraGainButton, &QPushButton::clicked, this, [this]() {
        m_Controller->updateRegisterFeature();
        QList<QStandardItem*> tmp = m_Controller->controllerModel()->findItems("Gain", Qt::MatchRecursive | Qt::MatchWrap);
        if (!tmp.isEmpty()) { m_Controller->onClicked(tmp.at(0)->index().siblingAtColumn(1)); }
         });
    connect(m_ExposureTimeButton, &QPushButton::clicked, this, [this]() {
        m_Controller->updateRegisterFeature();
        QList<QStandardItem*> tmp = m_Controller->controllerModel()->findItems("ExposureTimeAbs", Qt::MatchRecursive | Qt::MatchWrap);
        if (!tmp.isEmpty()) { m_Controller->onClicked(tmp.at(0)->index().siblingAtColumn(1)); }
        });
    connect(m_FramerateButton, &QPushButton::clicked, this, [this]() {
        m_Controller->updateRegisterFeature();
        QList<QStandardItem*> tmp = m_Controller->controllerModel()->findItems("AcquisitionFrameRateAbs", Qt::MatchRecursive | Qt::MatchWrap);
        if (!tmp.isEmpty()) { m_Controller->onClicked(tmp.at(0)->index().siblingAtColumn(1)); }
        });



    m_TextItem = new QGraphicsTextItem;
    QFont serifFont("Arial", 12, QFont::Bold);
    m_TextItem->setFont(serifFont);
    m_TextItem->setDefaultTextColor(Qt::red);

    //setMaximumSize(600, 600);
}

//void ViewerWidget::onSetMousePosInScene(const QPointF& pPoint)
//{
//    if (!m_PixmapItem->pixmap().isNull())
//    {        
//        int pixValue=m_PixmapItem->pixmap().toImage().pixel(
//            static_cast<int>(pPoint.x()),
//            static_cast<int>(pPoint.y()));
//        if (m_FormatLabel->text().contains("Mono8"))
//        {
//            //pixValue = pixValue & 0xFF;
//        }
//        else if (m_FormatLabel->text().contains("Mono12"))
//        {
//            //pixValue = pixValue & 0x0FFF;
//        }
//        else
//        {
//            m_CursorScenePosLabel->setText("MONO");
//            return;
//        }
//        m_CursorScenePosLabel->setText("(" + QString::number(static_cast<int>(pPoint.x())) + " , " +
//            QString::number(static_cast<int>(pPoint.y())) + " , " +
//            QString::number(pixValue,16) + ")");
//    }
//    else
//    {
//        m_CursorScenePosLabel->setText("N/A");
//    }
//}

void ViewerWidget::onSetMousePosInCMap(QMouseEvent* event)
{
    //or also use m_colorMap->keyAxis(), they are the same 
    double x = m_QCPcenterAxisRect->axis(QCPAxis::atBottom)->pixelToCoord(event->pos().x());
    double y = m_QCPcenterAxisRect->axis(QCPAxis::atLeft)->pixelToCoord(event->pos().y());
    //double z = m_colorMap->data()->data(x, y);
    //qDebug() << x << "," << std::floor(x + 0.5) << "," << y << "," << std::floor(y + 0.5) << "," << z;
    m_CursorScenePosLabel->setText("(" + QString::number(std::floor(x + 0.5)) + " , " +
        QString::number(std::floor(y + 0.5)) + " , " +
        QString::number(m_colorMap->data()->data(x, y)) + ")");
    
    /*tracer part*/
    {
        double bottomKey = m_bottomGraph->keyAxis()->pixelToCoord(event->pos().x());
        m_QCPtracerbottom->setGraphKey(bottomKey);
        bottomKey = m_bottomGraph->keyAxis()->pixelToCoord(m_QCPtracerbottom->position->pixelPosition().x());
        /*the y value is more self-contained than obtaining it from imgCThread, which depends on the availability of m_Crx*/
        double bottomVal = m_bottomGraph->valueAxis()->pixelToCoord(m_QCPtracerbottom->position->pixelPosition().y());

        m_QCPtraceTextbottom->setText(QString::number(bottomKey) +
            "(" + QString::number(bottomKey - m_bottomGraph->dataMainKey(0)) + ")" + "," +
            QString::number(bottomVal, 'e', 3));
    }
    {
        double leftKey = m_leftGraph->keyAxis()->pixelToCoord(event->pos().y());
        m_QCPtracerleft->setGraphKey(leftKey);
        leftKey = m_leftGraph->keyAxis()->pixelToCoord(m_QCPtracerleft->position->pixelPosition().y());
        /*the y value is more self-contained than obtaining it from imgCThread, which depends on the availability of m_Crx*/
        double leftVal = m_leftGraph->valueAxis()->pixelToCoord(m_QCPtracerleft->position->pixelPosition().x());

        m_QCPtraceTextleft->setText(QString::number(leftKey) +
            "(" + QString::number(leftKey - m_leftGraph->dataMainKey(0)) + ")" + "," +
            QString::number(leftVal, 'e', 3));
    }
    if (!m_bIsCameraRunning) { m_QCP->replot(); }
    //TODO: add criteria for trigger setting
        

    //qDebug() << bottomKey << "," << m_bottomGraph->valueAxis()->pixelToCoord(m_QCPtracer->position->pixelPosition().y()) <<
    //    "," << m_QCPtracer->position->pixelPosition()<< event->pos();
    /*qDebug() << m_QCPcenterAxisRect->axis(QCPAxis::atBottom)->range() << "," <<
        m_QCPcenterAxisRect->axis(QCPAxis::atLeft)->range();*/
}



void ViewerWidget::OnShowContextMenu(const QPoint& pos)
{
    m_ContextMenu->exec(QCursor::pos());
}

ViewerWidget::~ViewerWidget()
{
    /* save setting position and geometry from last session */
    //QSettings settings("Allied Vision", "Vimba Viewer");
    //settings.setValue("geometry", saveGeometry());
    //settings.setValue("state", saveState(0));

    /* If cam is open */
    if (!m_sCameraID.isEmpty())
    {
        //settings.setValue(m_sCameraID, m_SaveImageOption.ImageDestination_Edit->text());
        //if (!m_SaveName.isEmpty())
        //    settings.setValue(m_sCameraID + "SaveImageName", m_SaveName);

        //if (NULL != m_saveFileDialog)
        //{
        //    delete m_saveFileDialog;
        //    m_saveFileDialog = NULL;
        //}

        releaseBuffer();
        //delete m_pImgCThread; // NO: might need this? since need to explicitly release the sharedpointer in that class
        //no need for above since Qt will take care of that destruction
        m_QCP->~QCustomPlot(); // somehow need to call the destructor myself, otherwise the mouseevent from QCP will give memory exception
        m_pCam->Close();
    }
}

QString ViewerWidget::getCameraID () const
{
    return m_sCameraID;
}

bool ViewerWidget::getCamOpenStatus() const
{
    return m_bIsCamOpen;
}

CameraPtr ViewerWidget::getCameraPtr()
{
    return m_pCam;
}

bool ViewerWidget::isControlledCamera(const CameraPtr& cam) const
{
    return SP_ACCESS(cam) == SP_ACCESS(m_pCam);
}


bool  ViewerWidget::getAdjustPacketSizeMessage(QString& sMessage)
{
    if (m_Controller->isGigE())
    {
        if (VmbErrorSuccess == m_Controller->getTreeStatus())
        {
            sMessage = "Packet Size Adjusted:\t";
        }
        else
        {
            sMessage = "Failed To Adjust Packet Size!";
            sMessage.append(" Reason: " + Helper::mapReturnCodeToString(m_Controller->getTreeStatus()));
        }

        return true;
    }

    return false;
}

void ViewerWidget::onSetCurrentFPS(const QString& sFPS)
{
    m_FramerateButton->setText(QString::fromStdString(" FPS: ") + sFPS + " ");
}

void ViewerWidget::onResetFPS()
{
    SP_ACCESS(m_pFrameObs)->resetFrameCounter(false);
}

void ViewerWidget::onSetFrameCounter(const unsigned int& nNumberOfFrames)
{
    m_FramesLabel->setText("Frames: " + QString::number(nNumberOfFrames) + " ");
}


void ViewerWidget::onSetEventMessage(const QStringList& sMsg)
{
    m_InformationWindow->setEventMessage(sMsg);
}

void ViewerWidget::onSetDescription(const QString& sDesc)
{
    m_Description->setText(sDesc);
}


/*controller filter action*/
void ViewerWidget::textFilterChanged()
{

    QRegExp::PatternSyntax syntax = QRegExp::PatternSyntax(0);
    Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;

    QRegExp regExp(m_FilterPatternLineEdit->text(), caseSensitivity, syntax);

    /*ways to find index in model from text*/
    //QList<QStandardItem*> tt = m_Controller->controllerModel()->findItems("ExposureTimeAbs", Qt::MatchRecursive | Qt::MatchWrap);
    //
    //QString t0 = tt.at(0)->text();
    //QVariant t1 = tt.at(0)->data(0); //text and data(role=display) are the same, but the second return a qvariant
    //int t2 = tt.at(0)->row();
    //QModelIndex asd = tt.at(0)->index(); //can find QModelIndex directly from item
    //
    //QModelIndex tmp = m_Controller->controllerModel()->indexFromItem(tt.at(0));//or can call from QStandardItemModel
    //QVariant tmp0 = tmp.data(0);
    //QModelIndex t3 = tmp.siblingAtColumn(1);//can access siblings at different column at same row
    //QVariant as = t3.data();// in this case, we have the value of ExposureTimeAbs
    //
    ////or can use match method from QAbstractItemModel
    //QModelIndexList currentItems = m_Controller->controllerModel()->match(
    //    m_Controller->controllerModel()->index(0, 0),
    //    Qt::DisplayRole, QVariant::fromValue(QString("ExposureTimeAbs")), 1, Qt::MatchWrap | Qt::MatchRecursive);

    m_Controller->m_ProxyModel->setFilterRegExp(regExp);
    m_Controller->expandAll();
    m_Controller->updateUnRegisterFeature();
    m_Controller->updateRegisterFeature();
    m_FilterPatternLineEdit->setFocus();
    m_FilterPatternLineEdit->selectAll();
}

void ViewerWidget::onImageCalcStartStop(bool start)
{
    start ? m_pImgCThread->StartProcessing() : m_pImgCThread->StopProcessing();
}

/* display frames on viewer, the ultimate signal comes from ImageProcessingThread::run() in FrameObserver.cpp */
void ViewerWidget::onimageReadyFromCalc()
{
    m_colorMap->rescaleDataRange(true);
    m_QCPcenterAxisRect->axis(QCPAxis::atLeft)->setScaleRatio(m_QCPcenterAxisRect->axis(QCPAxis::atBottom), 1.0);
    QMouseEvent event(QMouseEvent::None, m_pImgCThread->mousePos(), Qt::NoButton, 0, 0);
    m_bottomGraph->rescaleValueAxis(true,true); //only enlarge y and scale corresponde to visible x
    m_bottomGraph->keyAxis()->setRange(m_colorMap->keyAxis()->range());
    m_leftGraph->rescaleValueAxis(true, true);
    
    m_leftGraph->keyAxis()->setRange(m_colorMap->valueAxis()->range());
    
    /*set the secondary relative axis*/
    m_QCPleftAxisRect->axis(QCPAxis::atLeft)->setRange(m_leftGraph->keyAxis()->range() - m_leftGraph->data()->at(0)->key);
    m_QCPbottomAxisRect->axis(QCPAxis::atBottom)->setRange(m_bottomGraph->keyAxis()->range() - m_bottomGraph->data()->at(0)->key);


    m_pImgCThread->mutex().lock();

    m_FormatButton->setText("Pixel Format: " + m_pImgCThread->format() + " ");
    auto [w, h] = m_pImgCThread->WidthHeight();
    m_ImageSizeButtonH->setText("Size H: " + QString::number(h));
    m_ImageSizeButtonW->setText(",W: " + QString::number(w) + " ");
    onSetMousePosInCMap(&event);
    m_QCP->replot();
    
    updateExposureTime();
    updateCameraGain();
    m_pImgCThread->mutex().unlock();
    
    

}

void ViewerWidget::SetCurrentScreenROI()
{
    auto [maxw, maxh] = m_pImgCThread->maxWidthHeight();
    QCPRange xr = m_QCPcenterAxisRect->axis(QCPAxis::atBottom)->range();
    QCPRange yr = m_QCPcenterAxisRect->axis(QCPAxis::atLeft)->range();
    if (xr.lower > 0 && xr.upper < maxw && yr.lower>0 && yr.upper < maxh)
    {
        int xlower = 2 * std::floor(xr.lower / 2);
        int xupper = 2 * std::ceil(xr.upper / 2);
        int ylower = 2 * std::floor(yr.lower / 2);
        int yupper = 2 * std::ceil(yr.upper / 2);
        xupper += (xupper - xlower) % 4 == 0 ? 0 : 2;
        yupper += (yupper - ylower) % 4 == 0 ? 0 : 2;
        int xw = (xupper - xlower) > 2 ? xupper - xlower : 4;
        int yw = (yupper - ylower) > 2 ? yupper - ylower : 4;
        emit m_Controller->acquisitionStartStop("AcquisitionStopWidthHeight");
        Sleep(5);//give some time for it to shut down
        /*first reset the value to full*/
        ResetFullROI(true);
        FeaturePtr pFeat;
        if (VmbErrorSuccess == m_pCam->GetFeatureByName("Width", pFeat))
        {
            auto tmp = pFeat->SetValue(xw);
            if (VmbErrorSuccess != tmp)
            {
                m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to x width " + QString::number(xw) + ", " + QString::number(tmp), VimbaViewerLogCategory_ERROR);
                return;
            }
        }
        if (VmbErrorSuccess == m_pCam->GetFeatureByName("Height", pFeat))
        {
            if (VmbErrorSuccess != pFeat->SetValue(yw))
            {
                m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to y height " + QString::number(yw), VimbaViewerLogCategory_ERROR);
                return;
            }
        }
        if (VmbErrorSuccess == m_pCam->GetFeatureByName("OffsetX", pFeat))
        {
            if (VmbErrorSuccess != pFeat->SetValue(xlower))
            {
                m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to lower bound of x " + QString::number(xlower), VimbaViewerLogCategory_ERROR);
                return;
            }
        }
        if (VmbErrorSuccess == m_pCam->GetFeatureByName("OffsetY", pFeat))
        {
            if (VmbErrorSuccess != pFeat->SetValue(ylower))
            {
                m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to lower bound of y " + QString::number(ylower), VimbaViewerLogCategory_ERROR);
                return;
            }
        }
        emit m_Controller->acquisitionStartStop("AcquisitionStart");
        //m_QCP->rescaleAxes();
    }
    else
    {
        m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to invalid region", VimbaViewerLogCategory_ERROR);
    }
}

void ViewerWidget::ResetFullROI(bool notStartReStart)
{
    if (!notStartReStart)
    {
        emit m_Controller->acquisitionStartStop("AcquisitionStopWidthHeight");
    }
    FeaturePtr pFeat;
    if (VmbErrorSuccess == m_pCam->GetFeatureByName("OffsetX", pFeat))
    {
        if (VmbErrorSuccess != pFeat->SetValue(0))
        {
            m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to lower bound of x", VimbaViewerLogCategory_ERROR);
            return;
        }
    }
    if (VmbErrorSuccess == m_pCam->GetFeatureByName("OffsetY", pFeat))
    {
        if (VmbErrorSuccess != pFeat->SetValue(0))
        {
            m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to lower bound of y", VimbaViewerLogCategory_ERROR);
            return;
        }
    }
    if (VmbErrorSuccess == m_pCam->GetFeatureByName("Width", pFeat))
    {
        auto tt = m_pImgCThread->maxWidthHeight();
        if (VmbErrorSuccess != pFeat->SetValue(m_pImgCThread->maxWidthHeight().first))
        {
            m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to x width", VimbaViewerLogCategory_ERROR);
            return;
        }
    }
    if (VmbErrorSuccess == m_pCam->GetFeatureByName("Height", pFeat))
    {
        if (VmbErrorSuccess != pFeat->SetValue(m_pImgCThread->maxWidthHeight().second))
        {
            m_InformationWindow->feedLogger("Logging", "Failed to set ROI due to y height", VimbaViewerLogCategory_ERROR);
            return;
        }
    }
    if (!notStartReStart)
    {
        emit m_Controller->acquisitionStartStop("AcquisitionStart");
    }
}

void ViewerWidget::onFullBitDepthImageReady(tFrameInfo mFullImageInfo)
{
    //// store a full bit depth image frame in case user wants to save to file
    //m_FullBitDepthImage = mFullImageInfo;

    //// save series of TIFF images using LibTif
    //if ((0 < m_nNumberOfFramesToSave) && m_Allow16BitMultiSave)
    //{
    //    m_SaveImageThread->start();
    //    ++m_nImageCounter;

    //    if (m_nImageCounter <= m_nNumberOfFramesToSave)
    //    {
    //        try
    //        {
    //            m_SaveImageThread->Enqueue(mFullImageInfo, m_nImageCounter);
    //        }
    //        catch (const std::bad_alloc& bex)
    //        {
    //            m_bIsRedHighlighted = false;

    //            ActionFreerun->setChecked(false);
    //            on_ActionFreerun_triggered();
    //            ActionFreerun->setEnabled(isStreamingAvailable());
    //            m_SaveImagesDialog->hide();
    //            delete m_SaveImagesDialog;
    //            m_SaveImagesDialog = NULL;
    //            m_SaveImageThread->StopProcessing();
    //            m_SaveImageThread->wait();
    //            m_bIsTriggeredByMultiSaveBtn = false;
    //            m_Allow16BitMultiSave = false;
    //        }
    //    }
    //}
}

void ViewerWidget::onFeedLogger(const QString& sMessage)
{
    m_InformationWindow->feedLogger("Logging", QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" + sMessage), VimbaViewerLogCategory_ERROR);
}

void ViewerWidget::updateExposureTime()
{
    m_pImgCThread->updateExposureTime();
    m_ExposureTimeButton->setText("Exposure time (ms): " + QString::number(m_pImgCThread->exposureTime() / 1.0e3, 'f', 3));
}

void ViewerWidget::updateCameraGain()
{
    m_pImgCThread->updateCameraGain();
    m_CameraGainButton->setText("Gain (dB): " + QString::number(m_pImgCThread->cameraGain() ));
}

void ViewerWidget::checkDisplayInterval()
{
    FeaturePtr pFeatMode;

    if (VmbErrorSuccess == m_pCam->GetFeatureByName("AcquisitionMode", pFeatMode))
    {
        std::string sValue("");
        if (VmbErrorSuccess == pFeatMode->GetValue(sValue))
        {
            /* display all received frames for SingleFrame and MultiFrame mode or if the user wants to have it */
            if (0 == sValue.compare("SingleFrame") || 0 == sValue.compare("MultiFrame") || m_bIsDisplayEveryFrame)
                SP_ACCESS(m_pFrameObs)->setDisplayInterval(0);
            /* display frame in a certain interval to save CPU consumption for continuous mode */
            else
                SP_ACCESS(m_pFrameObs)->setDisplayInterval(1);
        }
    }
}

bool ViewerWidget::isStreamingAvailable()
{
    AVT::VmbAPI::FeaturePtr pStreamIDFeature;
    m_pCam->GetFeatureByName("StreamID", pStreamIDFeature);
    return (NULL == pStreamIDFeature) ? false : true;
}

VmbError_t ViewerWidget::onPrepareCapture()
{
    FeaturePtr pFeature;
    VmbInt64_t nPayload = 0;
    QVector <FramePtr> frames;
    VmbError_t error = m_pCam->GetFeatureByName("PayloadSize", pFeature);
    VmbUint32_t nCounter = 0;
    if (VmbErrorSuccess == error)
    {
        error = pFeature->GetValue(nPayload);
        if (VmbErrorSuccess == error)
        {
            frames.resize(m_FrameBufferCount);

            bool bIsStreamingAvailable = isStreamingAvailable();

            if (bIsStreamingAvailable)
            {
                for (int i = 0; i < frames.size(); i++)
                {
                    try
                    {
                        frames[i] = FramePtr(new Frame(nPayload));
                        nCounter++;
                    }
                    catch (std::bad_alloc&)
                    {
                        frames.resize((VmbInt64_t)(nCounter * 0.7));
                        break;
                    }
                    /*this is the key part to set the frame thread start to receive signal*/
                    m_pFrameObs->Starting();
                    /*the start() will do a lot of overhead to create the thread and it eventually call run()*/

                    error = frames[i]->RegisterObserver(m_pFrameObs);
                    if (VmbErrorSuccess != error)
                    {
                        m_InformationWindow->feedLogger("Logging",
                            QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" + " RegisterObserver frame[" + QString::number(i) + "] Failed! Error: " + QString::number(error) + " " + Helper::mapReturnCodeToString(error)),
                            VimbaViewerLogCategory_ERROR);
                        return error;
                    }
                }

                for (int i = 0; i < frames.size(); i++)
                {
                    error = m_pCam->AnnounceFrame(frames[i]);
                    if (VmbErrorSuccess != error)
                    {
                        m_InformationWindow->feedLogger("Logging",
                            QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" + " AnnounceFrame [" + QString::number(i) + "] Failed! Error: " + QString::number(error) + " " + Helper::mapReturnCodeToString(error)),
                            VimbaViewerLogCategory_ERROR);
                        return error;
                    }
                }
            }

            if (VmbErrorSuccess == error)
            {
                error = m_pCam->StartCapture();
                if (VmbErrorSuccess != error)
                {
                    QString sMessage = " StartCapture Failed! Error: ";

                    if (0 != m_sAccessMode.compare(tr("(READ ONLY)")))
                        m_InformationWindow->feedLogger("Logging",
                            QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" + sMessage + QString::number(error) + " " + Helper::mapReturnCodeToString(error)),
                            VimbaViewerLogCategory_ERROR);
                    return error;
                }
            }

            if (bIsStreamingAvailable)
            {
                for (int i = 0; i < frames.size(); i++)
                {
                    error = m_pCam->QueueFrame(frames[i]);
                    if (VmbErrorSuccess != error)
                    {
                        m_InformationWindow->feedLogger("Logging",
                            QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" + " QueueFrame [" + QString::number(i) + "] Failed! Error: " + QString::number(error) + " " + Helper::mapReturnCodeToString(error)),
                            VimbaViewerLogCategory_ERROR);
                        return error;
                    }
                }
            }
        }
        else
        {
            m_InformationWindow->feedLogger("Logging",
                QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" + " GetValue [PayloadSize] Failed! Error: " + QString::number(error) + " " + Helper::mapReturnCodeToString(error)),
                VimbaViewerLogCategory_ERROR);
            return error;
        }
    }
    else
    {
        m_InformationWindow->feedLogger("Logging",
            QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" + " GetFeatureByName [PayloadSize] Failed! Error: " + QString::number(error) + " " + Helper::mapReturnCodeToString(error)),
            VimbaViewerLogCategory_ERROR);
        return error;
    }

    return error;
}



void ViewerWidget::on_ActionFreerun_triggered()
{
    VmbError_t error;
    FeaturePtr pFeat;

    checkDisplayInterval();

    /* update interpolation state after start */
    if (!m_Timer->isActive())
    {
        m_Timer->start(200);
    }


    /* ON */
    if (m_aStartStopCap->isChecked())
    {
        

        error = onPrepareCapture();
        if (VmbErrorSuccess != error)
        {
            m_bIsCameraRunning = false;
            emit acquisitionRunning(false);
            m_OperatingStatusLabel->setText(" Failed to start! Error: " + QString::number(error) + " " + Helper::mapReturnCodeToString(error));
            m_OperatingStatusLabel->setStyleSheet("background-color: rgb(196,0, 0); color: rgb(255,255,255)");
            m_aStartStopCap->setChecked(false);
            return;
        }

        error = m_pCam->GetFeatureByName("AcquisitionStart", pFeat);
        int nResult = m_sAccessMode.compare(tr("(READ ONLY)"));
        
        FeaturePtr pFeatFormat;
        auto error2 = m_pCam->GetFeatureByName("PixelFormat", pFeatFormat);
        QString Pixformat = m_Controller->getFeatureValue(pFeatFormat);
        int nResult2 = Pixformat.compare("Mono12Packed");
        
        if ((VmbErrorSuccess == error) && (0 != nResult) && (VmbErrorSuccess == error2) && (0 != nResult2))
        {
            SP_ACCESS(m_pFrameObs)->resetFrameCounter(true);

            // Do some GUI-related preparations before really starting (to avoid timing problems)
            m_OperatingStatusLabel->setText(" Running... ");
            m_OperatingStatusLabel->setStyleSheet("background-color: rgb(0,128, 0); color: rgb(255,255,255)");

            //if (ActionDisplayOptions->isEnabled())
            //    ActionDisplayOptions->setEnabled(false);

            //if (ActionSaveOptions->isEnabled())
            //    ActionSaveOptions->setEnabled(false);

            //if (ActionSaveImages->isEnabled() && (0 < m_SaveImageOption.NumberOfFrames_SpinBox->value()))
            //    ActionSaveImages->setEnabled(false);

            error = pFeat->RunCommand();

            if (VmbErrorSuccess == error)
            {
                if (m_bIsFirstStart)
                {
                    m_bIsFirstStart = false;
                }

                m_bHasJustStarted = true;
                m_bIsCameraRunning = true;
                emit acquisitionRunning(true);
            }
            else
            {
                m_bIsCameraRunning = false;
                emit acquisitionRunning(false);
                m_OperatingStatusLabel->setText(" Failed to execute AcquisitionStart! Error: " + QString::number(error) + " " + Helper::mapReturnCodeToString(error));
                m_OperatingStatusLabel->setStyleSheet("background-color: rgb(196,0, 0); color: rgb(255,255,255)");

                m_InformationWindow->feedLogger("Logging", QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" +
                    " RunCommand [AcquisitionStart] Failed! Error: " + QString::number(error) + " " +
                    Helper::mapReturnCodeToString(error)), VimbaViewerLogCategory_ERROR);

                //if (ActionDisplayOptions->isEnabled())
                //    ActionDisplayOptions->setEnabled(true);

                //if (ActionSaveOptions->isEnabled())
                //    ActionSaveOptions->setEnabled(true);

                //if (ActionSaveImages->isEnabled() && (0 < m_SaveImageOption.NumberOfFrames_SpinBox->value()))
                //    ActionSaveImages->setEnabled(true);

            }
        }
        else if ((VmbErrorSuccess == error2) && (0 == nResult2))
        {
            m_InformationWindow->feedLogger("Logging", "Please do NOT use Mono12Packed, we are not bandwidth limited and I am lazy to add that support", VimbaViewerLogCategory_ERROR);
        }
    }
    /* OFF */
    else
    {
        error = m_pCam->GetFeatureByName("AcquisitionStop", pFeat);
        if ((VmbErrorSuccess == error))
        {
            if (0 != m_sAccessMode.compare(tr("(READ ONLY)")))
                error = pFeat->RunCommand();

            

            if (VmbErrorSuccess == error)
            {
                m_bIsCameraRunning = false;
                emit acquisitionRunning(false);
                m_OperatingStatusLabel->setText(" Ready ");
                m_OperatingStatusLabel->setStyleSheet("background-color: rgb(0,0, 0); color: rgb(255,255,255)");

                releaseBuffer();
            }
            else
            {
                m_InformationWindow->feedLogger("Logging", QString(QTime::currentTime().toString("hh:mm:ss:zzz") + "\t" +
                    " RunCommand [AcquisitionStop] Failed! Error: " + QString::number(error) + " " +
                    Helper::mapReturnCodeToString(error)), VimbaViewerLogCategory_ERROR);
            }
        }


        //if (!ActionDisplayOptions->isEnabled())
        //    ActionDisplayOptions->setEnabled(true);

        //if (!ActionSaveOptions->isEnabled())
        //    ActionSaveOptions->setEnabled(true);

        //if (!ActionSaveImages->isEnabled() && (0 < m_SaveImageOption.NumberOfFrames_SpinBox->value()))
        //    ActionSaveImages->setEnabled(true);

        m_Controller->synchronizeEventFeatures();
    }
}

VmbError_t ViewerWidget::releaseBuffer()
{
    m_pFrameObs->Stopping();
    m_pImgCThread->StopProcessing();
    VmbError_t error = m_pCam->EndCapture();
    if (VmbErrorSuccess == error)
        error = m_pCam->FlushQueue();
    if (VmbErrorSuccess == error)
        error = m_pCam->RevokeAllFrames();

    return error;
}




VmbError_t ViewerWidget::getOpenError() const
{
    return m_OpenError;
}


/*this is for the command from tree controller*/
void ViewerWidget::onAcquisitionStartStop(const QString& sThisFeature)
{


    /* this is intended to stop and start the camera again since PixelFormat, Height and Width have been changed while camera running
    *  ignore this when the changing has been made while camera not running
    */
    if (((0 == sThisFeature.compare("AcquisitionStart")) && (m_bIsCameraRunning)))
    {
        m_aStartStopCap->setChecked(true);
        on_ActionFreerun_triggered(); //mainly for the gui and logger stuff
    }
    else if (sThisFeature.contains("AcquisitionStartFreerun"))
    {
        SP_ACCESS(m_pFrameObs)->resetFrameCounter(true);
        if (!m_bIsCameraRunning)
        {

            checkDisplayInterval();
            releaseBuffer();
            onPrepareCapture();

            m_aStartStopCap->setChecked(true);
            m_bIsCameraRunning = true;
            m_bHasJustStarted = true;
            emit acquisitionRunning(true);

            //if (ActionDisplayOptions->isEnabled())
            //    ActionDisplayOptions->setEnabled(false);

            //if (ActionSaveOptions->isEnabled())
            //    ActionSaveOptions->setEnabled(false);

            /* if save images settings set, and acquisition starts */
            //if ((0 < m_SaveImageOption.NumberOfFrames_SpinBox->value()) && ActionSaveImages->isEnabled())
            //{
            //    ActionSaveImages->setEnabled(false);
            //    m_nImageCounter = 0;
            //    m_nNumberOfFramesToSave = 0;
            //}

            m_OperatingStatusLabel->setText(" Running... ");
            m_OperatingStatusLabel->setStyleSheet("background-color: rgb(0,128, 0); color: rgb(255,255,255)");
        }
    }
    else if (sThisFeature.contains("AcquisitionStopFreerun"))
    {
        if (m_bIsCameraRunning)
        {
            
            releaseBuffer();
            m_aStartStopCap->setChecked(false);
            if (m_bIsViewerWindowClosing)
                on_ActionFreerun_triggered();

            m_bIsCameraRunning = false;
            emit acquisitionRunning(false);

            //if (!ActionSaveOptions->isEnabled())
            //    ActionSaveOptions->setEnabled(true);

            if (!m_aStartStopCap->isEnabled())
                m_aStartStopCap->setEnabled(isStreamingAvailable());

            //if (!ActionDisplayOptions->isEnabled())
            //    ActionDisplayOptions->setEnabled(true);

            /* if save images running, and acquisition stops */
            //if ((0 < m_SaveImageOption.NumberOfFrames_SpinBox->value()) && !ActionSaveImages->isEnabled())
            //{
            //    ActionSaveImages->setEnabled(true);
            //}

            m_Controller->synchronizeEventFeatures();
        }

        m_OperatingStatusLabel->setText(" Ready ");
        m_OperatingStatusLabel->setStyleSheet("background-color: rgb(0,0, 0); color: rgb(255,255,255)");
    }
    else if (((0 == sThisFeature.compare("AcquisitionStop")) && (m_bIsCameraRunning)) ||
        (sThisFeature.contains("AcquisitionStopWidthHeight")))
    {
        if (m_bIsCameraRunning)
        {
            m_aStartStopCap->setChecked(false);
            on_ActionFreerun_triggered();

            /* use this for GigE, so you can change the W/H "on the fly" */
            if (0 == sThisFeature.compare("AcquisitionStopWidthHeight"))
            {
                m_bIsCameraRunning = true;
                emit acquisitionRunning(true);
            }
        }

        // update state of full bit depth image transfer flag in case pixel format has changed
        //on_ActionAllow16BitTiffSaving_triggered();
    }

    // update state of full bit depth image transfer flag in case pixel format has changed
    //on_ActionAllow16BitTiffSaving_triggered();
}
