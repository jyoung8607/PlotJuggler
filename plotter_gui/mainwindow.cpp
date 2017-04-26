#include <functional>
#include <stdio.h>
#include <set>
#include <QMouseEvent>
#include <QDebug>
#include <numeric>
#include <QMimeData>
#include <QMenu>
#include <QStringListModel>
#include <qwt_plot_canvas.h>
#include <QDomDocument>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMessageBox>
#include <QStringRef>
#include <QThread>
#include <QPluginLoader>
#include <QSettings>
#include <QWindow>
#include <QInputDialog>
#include <QCommandLineParser>
#include <QMovie>
#include <QScrollBar>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "busydialog.h"
#include "busytaskdialog.h"
#include "filterablelistwidget.h"
#include "tabbedplotwidget.h"
#include "selectlistdialog.h"
#include "aboutdialog.h"


MainWindow::MainWindow(const QCommandLineParser &commandline_parser, QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    _undo_shortcut(QKeySequence(Qt::CTRL + Qt::Key_Z), this),
    _redo_shortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z), this),
    _current_streamer(nullptr),
    _disable_undo_logging(false),
    _tracker_param( CurveTracker::VALUE )
{
    QLocale::setDefault(QLocale::c()); // set as default

    _test_option = (commandline_parser.isSet("test"));

    _curvelist_widget = new FilterableListWidget(this);
    _streamer_signal_mapper = new QSignalMapper(this);

    ui->setupUi(this);

    if( commandline_parser.isSet("buffer_size"))
    {
        int buffer_size = std::max(10, commandline_parser.value("buffer_size").toInt() );
        ui->streamingSpinBox->setMaximum(buffer_size);
    }

    {
        QIcon icon(":/icons/resources/office_chart_line_stacked.png");
        if (!icon.isNull())
        {
            this->setWindowIcon(icon);
            QApplication::setWindowIcon(icon);
        }
    }

    connect( _curvelist_widget->getTable()->verticalScrollBar(), &QScrollBar::sliderMoved,
             this, &MainWindow::updateLeftTableValues );

    connect( _curvelist_widget, &FilterableListWidget::hiddenItemsChanged,
             this, &MainWindow::updateLeftTableValues );

    connect(_curvelist_widget, &FilterableListWidget::deleteCurve,
            this, &MainWindow::deleteDataOfSingleCurve );

    connect( ui->timeSlider, &RealSlider::realValueChanged,
             this, &MainWindow::onTimeSlider_valueChanged );

    _main_tabbed_widget = new TabbedPlotWidget( this, NULL, _mapped_plot_data, this);

    ui->centralLayout->insertWidget(0, _main_tabbed_widget);
    ui->leftLayout->addWidget( _curvelist_widget );

    ui->splitter->setCollapsible(0,true);
    ui->splitter->setStretchFactor(0,2);
    ui->splitter->setStretchFactor(1,6);

    connect( ui->splitter, SIGNAL(splitterMoved(int,int)), SLOT(onSplitterMoved(int,int)) );

    createActions();

    loadPlugins( QCoreApplication::applicationDirPath() );
    loadPlugins("/usr/local/PlotJuggler/plugins");

    _undo_timer.start();

    // save initial state
    onUndoableChange();

    _replot_timer = new QTimer(this);
    connect(_replot_timer, &QTimer::timeout, this, &MainWindow::updateDataAndReplot);

    ui->menuFile->setToolTipsVisible(true);
    ui->horizontalSpacer->changeSize(0,0, QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->streamingLabel->setHidden(true);
    ui->streamingSpinBox->setHidden(true);

    this->setMenuBar(ui->menuBar);
    ui->menuBar->setNativeMenuBar(false);

    connect(_streamer_signal_mapper, SIGNAL(mapped(QString)),
            this, SLOT(onActionLoadStreamer(QString)) );

    ui->actionDeleteAllData->setEnabled( _test_option );

    if( _test_option )
    {
        buildDummyData();
    }

    bool file_loaded = false;
    if( commandline_parser.isSet("datafile"))
    {
        QMetaObject::invokeMethod(this, "onActionLoadDataFileImpl",
                                  Q_ARG(QString, commandline_parser.value("datafile")) );
        file_loaded = true;
    }
    if( commandline_parser.isSet("layout"))
    {
        QMetaObject::invokeMethod(this, "onActionLoadLayoutFromFile",
                                  Q_ARG(QString, commandline_parser.value("layout")),
                                  Q_ARG(bool, file_loaded ) );
    }

    QSettings settings( "IcarusTechnology", "PlotJuggler");
    restoreGeometry(settings.value("MainWindow.geometry").toByteArray());

    bool activate_grid = settings.value("MainWindow.activateGrid", false).toBool();
    ui->pushButtonActivateGrid->setChecked(activate_grid);

    int streaming_buffer_value = settings.value("MainWindow.streamingBufferValue", 5).toInt();
    ui->streamingSpinBox->setValue(streaming_buffer_value);

    bool datetime_display  = settings.value("MainWindow.dateTimeDisplay", false).toBool();
    ui->pushButtonUseDateTime->setChecked( datetime_display );

    ui->widgetOptions->setVisible( ui->pushButtonOptions->isChecked() );
    ui->line->setVisible( ui->pushButtonOptions->isChecked() );

    //----------------------------------------------------------
    QIcon trackerIconA, trackerIconB, trackerIconC;

    trackerIconA.addFile(QStringLiteral(":/icons/resources/line_tracker.png"), QSize(36, 36));
    trackerIconB.addFile(QStringLiteral(":/icons/resources/line_tracker_1.png"), QSize(36, 36));
    trackerIconC.addFile(QStringLiteral(":/icons/resources/line_tracker_a.png"), QSize(36, 36));

    _tracker_button_icons[CurveTracker::LINE_ONLY]  = trackerIconA;
    _tracker_button_icons[CurveTracker::VALUE]      = trackerIconB;
    _tracker_button_icons[CurveTracker::VALUE_NAME] = trackerIconC;

    int tracker_setting = settings.value("MainWindow.timeTrackerSetting", (int)CurveTracker::VALUE ).toInt();
    _tracker_param = static_cast<CurveTracker::Parameter>(tracker_setting);

    ui->pushButtonTimeTracker->setIcon( _tracker_button_icons[_tracker_param] );

    forEachWidget( [&](PlotWidget* plot) {
        plot->configureTracker(_tracker_param);
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::onUndoableChange()
{
    if(_disable_undo_logging) return;

    int elapsed_ms = _undo_timer.restart();

    // overwrite the previous
    if( elapsed_ms < 100)
    {
        if( _undo_states.empty() == false)
            _undo_states.pop_back();
    }

    while( _undo_states.size() >= 100 ) _undo_states.pop_front();
    _undo_states.push_back( xmlSaveState() );
    _redo_states.clear();
    //  qDebug() << "undo " << _undo_states.size();
}


void MainWindow::onRedoInvoked()
{
    _disable_undo_logging = true;
    if( _redo_states.size() > 0)
    {
        QDomDocument state_document = _redo_states.back();
        while( _undo_states.size() >= 100 ) _undo_states.pop_front();
        _undo_states.push_back( state_document );
        _redo_states.pop_back();

        xmlLoadState( state_document );
    }
    _disable_undo_logging = false;
}


void MainWindow::updateLeftTableValues()
{
    const auto& table = _curvelist_widget->getTable();

    if( table->isColumnHidden(1) == false)
    {
        const int vertical_height = table->visibleRegion().boundingRect().height();

        for (int row = 0; row < _curvelist_widget->rowCount(); row++)
        {
            int vertical_pos = table->rowViewportPosition(row);
            if( vertical_pos < 0 || table->isRowHidden(row) ){ continue; }
            if( vertical_pos > vertical_height){ break; }

            const std::string name = table->item(row,0)->text().toStdString();
            auto it = _mapped_plot_data.numeric.find(name);
            if( it !=  _mapped_plot_data.numeric.end())
            {
                nonstd::optional<PlotData::TimeType> value;
                PlotDataPtr data = it->second;

                double num = 0.0;
                bool valid = false;

                if( _tracker_time < std::numeric_limits<double>::max())
                {
                    auto value = data->getYfromX( _tracker_time );
                    if(value){
                        valid = true;
                        num = value.value();
                    }
                }
                else{
                    if( data->size() > 0) {
                        valid = true;
                        num = (data->at( data->size()-1 )).y;
                    }
                }
                if( valid)
                {
                    QString num_text = QString::number( num, 'f', 3);
                    if(num_text.contains('.'))
                    {
                        int idx = num_text.length() -1;
                        while( num_text[idx] == '0' )
                        {
                            num_text[idx] = ' ';
                            idx--;
                        }
                        if(  num_text[idx] == '.') num_text[idx] = ' ';
                    }
                    table->item(row,1)->setText(num_text + ' ');
                }
            }
        }
    }
}


void MainWindow::onTrackerMovedFromWidget(QPointF relative_pos)
{
    _tracker_time = relative_pos.x() + _time_offset.get();

    auto prev = ui->timeSlider->blockSignals(true);
    ui->timeSlider->setRealValue( relative_pos.x() );
    ui->timeSlider->blockSignals(prev);

    onTrackerTimeUpdated( _tracker_time );
}

void MainWindow::onTimeSlider_valueChanged(double relative_time)
{
    _tracker_time = relative_time + _time_offset.get();
    onTrackerTimeUpdated( _tracker_time );
}

void MainWindow::onTrackerTimeUpdated(double absolute_time)
{
    updatedDisplayTime();
    updateLeftTableValues();

    for ( auto it: _state_publisher)
    {
        it.second->updateState( &_mapped_plot_data, absolute_time);
    }

    forEachWidget( [&](PlotWidget* plot)
    {
        plot->setTrackerPosition( _tracker_time );
        plot->replot();
    } );
}

void MainWindow::createTabbedDialog(PlotMatrix* first_tab)
{
    SubWindow* window = new SubWindow(first_tab, _mapped_plot_data, this );
    Qt::WindowFlags flags = window->windowFlags();
    window->setWindowFlags( flags | Qt::SubWindow );

    const char prefix[] = "Window ";
    int window_number = 1;

    bool number_taken = true;
    while( number_taken )
    {
        number_taken = false;
        for (const auto& floating_window: _floating_window )
        {
            QString win_title = floating_window->windowTitle();
            win_title.remove(0, sizeof(prefix)-1 );
            int num = win_title.toInt();

            if (num == window_number)
            {
                number_taken = true;
                window_number++;
                break;
            }
        }
    }

    window->setWindowTitle( QString(prefix) + QString::number(window_number));

    connect( window, SIGNAL(destroyed(QObject*)),    this,  SLOT(onFloatingWindowDestroyed(QObject*)) );
    connect( window, SIGNAL(closeRequestedByUser()), this,  SLOT(onUndoableChange()) );

    _floating_window.push_back( window );

    window->tabbedWidget()->setStreamingMode( isStreamingActive() );

    window->setAttribute( Qt::WA_DeleteOnClose, true );
    window->show();
    window->activateWindow();
    window->raise();

    if (this->signalsBlocked() == false) onUndoableChange();
}


void MainWindow::createActions()
{
    _undo_shortcut.setContext(Qt::ApplicationShortcut);
    _redo_shortcut.setContext(Qt::ApplicationShortcut);

    connect( &_undo_shortcut, &QShortcut::activated, this, &MainWindow::onUndoInvoked );
    connect( &_redo_shortcut, &QShortcut::activated, this, &MainWindow::onRedoInvoked );

    //---------------------------------------------

    connect( ui->actionSaveLayout, &QAction::triggered,        this, &MainWindow::onActionSaveLayout );
    connect(ui->actionLoadLayout, &QAction::triggered,         this, &MainWindow::onActionLoadLayout );
    connect(ui->actionLoadData, &QAction::triggered,           this, &MainWindow::onActionLoadDataFile );
    connect(ui->actionLoadRecentDatafile, &QAction::triggered, this, &MainWindow::onActionReloadDataFileFromSettings );
    connect(ui->actionLoadRecentLayout, &QAction::triggered,   this, &MainWindow::onActionReloadRecentLayout );
    connect(ui->actionDeleteAllData, &QAction::triggered,      this, &MainWindow::onDeleteLoadedData );

    //---------------------------------------------

    QSettings settings( "IcarusTechnology", "PlotJuggler");
    if( settings.contains("MainWindow.recentlyLoadedDatafile") )
    {
        QString filename = settings.value("MainWindow.recentlyLoadedDatafile").toString();
        ui->actionLoadRecentDatafile->setText( "Load data from: " + filename);
        ui->actionLoadRecentDatafile->setEnabled( true );
    }
    else{
        ui->actionLoadRecentDatafile->setEnabled( false );
    }

    if( settings.contains("MainWindow.recentlyLoadedLayout") )
    {
        QString filename = settings.value("MainWindow.recentlyLoadedLayout").toString();
        ui->actionLoadRecentLayout->setText( "Load layout from: " + filename);
        ui->actionLoadRecentLayout->setEnabled( true );
    }
    else{
        ui->actionLoadRecentLayout->setEnabled( false );
    }
}

void MainWindow::loadPlugins(QString directory_name)
{
    static std::set<QString> loaded_plugins;

    QDir pluginsDir( directory_name );

    for (QString filename: pluginsDir.entryList(QDir::Files))
    {
        QFileInfo fileinfo(filename);
        if( fileinfo.suffix() != "so" && fileinfo.suffix() != "dll"){
            continue;
        }

        if( loaded_plugins.find( filename ) != loaded_plugins.end())
        {
            continue;
        }

        QPluginLoader pluginLoader(pluginsDir.absoluteFilePath(filename));

        QObject *plugin = pluginLoader.instance();
        if (plugin)
        {
            DataLoader *loader        = qobject_cast<DataLoader *>(plugin);
            StatePublisher *publisher = qobject_cast<StatePublisher *>(plugin);
            DataStreamer *streamer    =  qobject_cast<DataStreamer *>(plugin);

            if (loader)
            {
                qDebug() << filename << ": is a DataLoader plugin";
                if( !_test_option && loader->isDebugPlugin())
                {
                    qDebug() << filename << "...but will be ignored unless the argument -t is used.";
                }
                else{
                    loaded_plugins.insert( loader->name() );
                    _data_loader.insert( std::make_pair( loader->name(), loader) );
                }
            }
            else if (publisher)
            {
                qDebug() << filename << ": is a StatePublisher plugin";
                if( !_test_option && publisher->isDebugPlugin())
                {
                    qDebug() << filename << "...but will be ignored unless the argument -t is used.";
                }
                else
                {
                    loaded_plugins.insert( publisher->name() );
                    _state_publisher.insert( std::make_pair(publisher->name(), publisher) );

                    publisher->setParentMenu( ui->menuPublishers );

                    QAction* activatePublisher = new QAction( publisher->name() , this);
                    activatePublisher->setCheckable(true);
                    activatePublisher->setChecked(false);
                    ui->menuPublishers->setEnabled(true);
                    ui->menuPublishers->addAction(activatePublisher);

                    connect(activatePublisher, &QAction::toggled,
                           [=](bool enable) { publisher->setEnabled( enable ); } );
                }
            }
            else if (streamer)
            {
                qDebug() << filename << ": is a DataStreamer plugin";
                if( !_test_option && streamer->isDebugPlugin())
                {
                    qDebug() << filename << "...but will be ignored unless the argument -t is used.";
                }
                else{
                    QString name(streamer->name());
                    loaded_plugins.insert( name );
                    _data_streamer.insert( std::make_pair(name , streamer ) );

                    QAction* startStreamer = new QAction(QString("Start: ") + name, this);
                    ui->menuStreaming->setEnabled(true);
                    ui->menuStreaming->addAction(startStreamer);

                    streamer->setParentMenu( ui->menuStreaming );

                    connect(startStreamer, SIGNAL(triggered()),
                            _streamer_signal_mapper, SLOT(map()) );
                    _streamer_signal_mapper->setMapping(startStreamer, name );
                }
            }
        }
        else{
            if( pluginLoader.errorString().contains("is not an ELF object") == false)
            {
                qDebug() << filename << ": " << pluginLoader.errorString();
            }
        }
    }
}

void MainWindow::buildDummyData()
{
    size_t SIZE = 100*1000;

    QStringList  words_list;
    words_list << "world/siam" << "world/tre" << "walk/piccoli" << "walk/porcellin"
               << "fly/high/mai" << "fly/high/nessun" << "fly/low/ci" << "fly/low/dividera";

    for(auto& word: words_list){
        _curvelist_widget->addItem( new QTableWidgetItem(word) );
    }

    for( const QString& name: words_list)
    {
        double A =  6* ((double)qrand()/(double)RAND_MAX)  - 3;
        double B =  3* ((double)qrand()/(double)RAND_MAX)  ;
        double C =  3* ((double)qrand()/(double)RAND_MAX)  ;
        double D =  20* ((double)qrand()/(double)RAND_MAX)  ;

        PlotDataPtr plot ( new PlotData( name.toStdString().c_str() ) );

        double t = 0;
        for (unsigned indx=0; indx<SIZE; indx++)
        {
            t += 0.001;
            plot->pushBack( PlotData::Point( t,  A*sin(B*t + C) + D*t*0.02 ) ) ;
        }
        _mapped_plot_data.numeric.insert( std::make_pair( name.toStdString(), plot) );
    }

    //---------------------------------------
    PlotDataPtr sin_plot ( new PlotData( "_sin" ) );
    PlotDataPtr cos_plot ( new PlotData( "_cos" ) );

    double t = 0;
    for (unsigned indx=0; indx<SIZE; indx++)
    {
        t += 0.001;
        sin_plot->pushBack( PlotData::Point( t,  1.0*sin(t*0.4) ) ) ;
        cos_plot->pushBack( PlotData::Point( t,  2.0*cos(t*0.4) ) ) ;
    }

    _mapped_plot_data.numeric.insert( std::make_pair( sin_plot->name(), sin_plot) );
    _mapped_plot_data.numeric.insert( std::make_pair( cos_plot->name(), cos_plot) );

    _curvelist_widget->addItem( new QTableWidgetItem(sin_plot->name().c_str()) );
    _curvelist_widget->addItem( new QTableWidgetItem(cos_plot->name().c_str()) );
    //--------------------------------------

    updateTimeSlider();

    _curvelist_widget->updateFilter();

    forEachWidget( [](PlotWidget* plot) {
        plot->reloadPlotData();
    } );
}

void MainWindow::onSplitterMoved(int , int )
{
    QList<int> sizes = ui->splitter->sizes();
    int maxLeftWidth = _curvelist_widget->maximumWidth();
    int totalWidth = sizes[0] + sizes[1];

    if( sizes[0] > maxLeftWidth)
    {
        sizes[0] = maxLeftWidth;
        sizes[1] = totalWidth - maxLeftWidth;
        ui->splitter->setSizes(sizes);
    }
}

void MainWindow::resizeEvent(QResizeEvent *)
{
    onSplitterMoved( 0, 0 );
}


void MainWindow::onPlotAdded(PlotWidget* plot)
{
    connect( plot, &PlotWidget::undoableChange,
             this, &MainWindow::onUndoableChange );

    connect( plot, &PlotWidget::trackerMoved,
             this, &MainWindow::onTrackerMovedFromWidget);

    connect( plot, &PlotWidget::swapWidgetsRequested,
             this, &MainWindow::onSwapPlots);

    connect( this, &MainWindow::requestRemoveCurveByName,
             plot, &PlotWidget::removeCurve) ;

    connect( &_time_offset, SIGNAL( valueChanged(double)),
             plot, SLOT(on_changeTimeOffset(double)) );

    plot->on_changeTimeOffset( _time_offset.get() );
    plot->activateGrid( ui->pushButtonActivateGrid->isChecked() );
    plot->enableTracker( !isStreamingActive() );
    plot->configureTracker( _tracker_param );
}

void MainWindow::onPlotMatrixAdded(PlotMatrix* matrix)
{
    connect( matrix, &PlotMatrix::plotAdded,      this, &MainWindow:: onPlotAdded);
    connect( matrix, &PlotMatrix::undoableChange, this, &MainWindow:: onUndoableChange );
}

QDomDocument MainWindow::xmlSaveState() const
{
    QDomDocument doc;
    QDomProcessingInstruction instr =
            doc.createProcessingInstruction("xml", "version='1.0' encoding='UTF-8'");

    doc.appendChild(instr);

    QDomElement root = doc.createElement( "root" );

    QDomElement main_area =_main_tabbed_widget->xmlSaveState(doc);
    root.appendChild( main_area );

    for (SubWindow* floating_window: _floating_window)
    {
        QDomElement tabbed_area = floating_window->tabbedWidget()->xmlSaveState(doc);
        root.appendChild( tabbed_area );
    }

    doc.appendChild(root);

    QDomElement relative_time = doc.createElement( "use_relative_time_offset" );
    relative_time.setAttribute("enabled", ui->pushButtonRemoveTimeOffset->isChecked() );
    root.appendChild( relative_time );

    return doc;
}

bool MainWindow::xmlLoadState(QDomDocument state_document)
{
    QDomElement root = state_document.namedItem("root").toElement();
    if ( root.isNull() ) {
        qWarning() << "No <root> element found at the top-level of the XML file!";
        return false;
    }

    QDomElement tabbed_area;

    size_t num_floating = 0;

    for (  tabbed_area = root.firstChildElement(  "tabbed_widget" )  ;
           tabbed_area.isNull() == false;
           tabbed_area = tabbed_area.nextSiblingElement( "tabbed_widget" ) )
    {
        if( tabbed_area.attribute("parent").compare("main_window") != 0)
        {
            num_floating++;
        }
    }

    // add windows if needed
    while( _floating_window.size() < num_floating )
    {
        createTabbedDialog( NULL );
    }

    while( _floating_window.size() > num_floating ){
        QMainWindow* window =  _floating_window.back();
        _floating_window.pop_back();
        window->deleteLater();
    }

    //-----------------------------------------------------
    size_t index = 0;

    for (  tabbed_area = root.firstChildElement(  "tabbed_widget" )  ;
           tabbed_area.isNull() == false;
           tabbed_area = tabbed_area.nextSiblingElement( "tabbed_widget" ) )
    {
        if( tabbed_area.attribute("parent").compare("main_window") == 0)
        {
            _main_tabbed_widget->xmlLoadState( tabbed_area );
        }
        else{
            _floating_window[index++]->tabbedWidget()->xmlLoadState( tabbed_area );
        }
    }

    QDomElement relative_time = root.firstChildElement( "use_relative_time_offset" );
    if( !relative_time.isNull())
    {
        bool remove_offset = (relative_time.attribute("enabled") == QString("1"));
        ui->pushButtonRemoveTimeOffset->setChecked(remove_offset);
    }
    onLayoutChanged();

    return true;
}

void MainWindow::onActionSaveLayout()
{
    QDomDocument doc = xmlSaveState();

    if( _loaded_datafile.isEmpty() == false)
    {
        QDomElement root = doc.namedItem("root").toElement();
        QDomElement previously_loaded_datafile =  doc.createElement( "previouslyLoadedDatafile" );
        QDomText textNode = doc.createTextNode( _loaded_datafile );
        previously_loaded_datafile.appendChild( textNode );
        root.appendChild( previously_loaded_datafile );
    }
    if( _current_streamer )
    {
        QDomElement root = doc.namedItem("root").toElement();
        QDomElement loaded_streamer =  doc.createElement( "previouslyLoadedStreamer" );
        QDomText textNode = doc.createTextNode( _current_streamer->name() );
        loaded_streamer.appendChild( textNode );
        root.appendChild( loaded_streamer );
    }

    QSettings settings( "IcarusTechnology", "PlotJuggler");

    QString directory_path  = settings.value("MainWindow.lastLayoutDirectory",
                                             QDir::currentPath() ). toString();

    QFileDialog saveDialog;
    saveDialog.setAcceptMode(QFileDialog::AcceptSave);
    saveDialog.setDefaultSuffix("xml");
    saveDialog.setDirectory(directory_path);
    saveDialog.exec();

    if(saveDialog.result() != QDialog::Accepted || saveDialog.selectedFiles().empty())
    {
        return;
    }

    QString fileName = saveDialog.selectedFiles().first();

    if (fileName.isEmpty())
        return;

    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file);
        stream << doc.toString() << endl;
    }
}

void MainWindow::deleteDataOfSingleCurve(const QString& curve_name)
{
    auto plot_curve = _mapped_plot_data.numeric.find( curve_name.toStdString() );
    if( plot_curve == _mapped_plot_data.numeric.end())
    {
        return;
    }

    _mapped_plot_data.numeric.erase( plot_curve );

    auto rows_to_remove = _curvelist_widget->findRowsByName( curve_name );
    for(int row : rows_to_remove)
    {
        _curvelist_widget->removeRow(row);
    }

    emit requestRemoveCurveByName( curve_name );


    if( _curvelist_widget->rowCount() == 0)
    {
        ui->actionDeleteAllData->setEnabled( false );
    }
}


void MainWindow::onDeleteLoadedData()
{
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(0, tr("Warning"),
                                  tr("Do you really want to remove the loaded data?\n"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No );
    if( reply == QMessageBox::No ) {
        return;
    }

    _mapped_plot_data.numeric.clear();
    _mapped_plot_data.user_defined.clear();

    _curvelist_widget->clear();

    forEachWidget( [](PlotWidget* plot) {
        plot->detachAllCurves();
    } );

    ui->actionDeleteAllData->setEnabled( false );

}

void MainWindow::onActionLoadDataFile(bool reload_from_settings)
{
    if( _data_loader.empty())
    {
        QMessageBox::warning(0, tr("Warning"),
                             tr("No plugin was loaded to process a data file\n") );
        return;
    }

    QSettings settings( "IcarusTechnology", "PlotJuggler");

    QString file_extension_filter;

    std::set<QString> extensions;

    for (auto& it: _data_loader)
    {
        DataLoader* loader = it.second;
        for (QString extension: loader->compatibleFileExtensions() )
        {
            extensions.insert( extension.toLower() );
        }
    }

    for (auto it = extensions.begin(); it != extensions.end(); it++)
    {
        file_extension_filter.append( QString(" *.") + *it );
    }

    QString directory_path = settings.value("MainWindow.lastDatafileDirectory", QDir::currentPath() ).toString();

    QString filename;
    if( reload_from_settings && settings.contains("MainWindow.recentlyLoadedDatafile") )
    {
        filename = settings.value("MainWindow.recentlyLoadedDatafile").toString();
    }
    else{
        filename = QFileDialog::getOpenFileName(this, "Open Datafile",
                                                directory_path,
                                                file_extension_filter);
    }

    if (filename.isEmpty()) {
        return;
    }

    directory_path = QFileInfo(filename).absolutePath();

    settings.setValue("MainWindow.lastDatafileDirectory", directory_path);
    settings.setValue("MainWindow.recentlyLoadedDatafile", filename);

    ui->actionLoadRecentDatafile->setText("Load data from: " + filename);

    onActionLoadDataFileImpl(filename, false );
}

void MainWindow::importPlotDataMap(const PlotDataMap& new_data, bool delete_older)
{
    // overwrite the old user_defined map
    _mapped_plot_data.user_defined = new_data.user_defined;

    for (auto& it: new_data.numeric)
    {
        const std::string& name  = it.first;
        PlotDataPtr plot  = it.second;
        auto plot_with_same_name = _mapped_plot_data.numeric.find(name);

        // this is a new plot
        if( plot_with_same_name == _mapped_plot_data.numeric.end() )
        {
            _curvelist_widget->addItem( new QTableWidgetItem( name.c_str() ) );
            _mapped_plot_data.numeric.insert( std::make_pair(name, plot) );
        }
        else{ // a plot with the same name existed already, overwrite it
            plot_with_same_name->second = plot;
        }
    }

    if( delete_older && _mapped_plot_data.numeric.size() > new_data.numeric.size() )
    {
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(0, tr("Warning"),
                                      tr("Do you want to remove the previously loaded data?\n"),
                                      QMessageBox::Yes | QMessageBox::No,
                                      QMessageBox::Yes );
        if( reply == QMessageBox::Yes )
        {
            std::vector<std::string> data_to_remove;

            for (auto& it: _mapped_plot_data.numeric )
            {
                auto& name = it.first;
                if( new_data.numeric.find( name ) == new_data.numeric.end() ){
                    data_to_remove.push_back(name);
                }
            }
            for (auto& to_remove: data_to_remove )
            {
                this->deleteDataOfSingleCurve( QString( to_remove.c_str() ) );
            }
        }
    }

    forEachWidget( [](PlotWidget* plot) {
        plot->reloadPlotData();
    } );

    updateTimeSlider();
}

bool MainWindow::isStreamingActive() const
{
    return ui->pushButtonStreaming->isChecked();
}

void MainWindow::onActionLoadDataFileImpl(QString filename, bool reuse_last_timeindex )
{
    const QString extension = QFileInfo(filename).suffix().toLower();

    DataLoader* loader = nullptr;

    typedef std::map<QString,DataLoader*>::iterator MapIterator;

    std::vector<MapIterator> compatible_loaders;

    for (MapIterator it = _data_loader.begin(); it != _data_loader.end(); it++)
    {
        DataLoader* data_loader = it->second;
        std::vector<const char*> extensions = data_loader->compatibleFileExtensions();

        for(auto& ext: extensions){

            if( extension == QString(ext).toLower()){
                compatible_loaders.push_back( it );
                break;
            }
        }
    }

    if( compatible_loaders.size() == 1)
    {
        loader = compatible_loaders.front()->second;
    }
    else{
        static QString last_plugin_name_used;

        QStringList names;
        for (auto cl: compatible_loaders)
        {
            const auto& name = cl->first;

            if( name == last_plugin_name_used ){
                names.push_front( name );
            }
            else{
                names.push_back( name );
            }
        }

        bool ok;
        QString plugin_name = QInputDialog::getItem(this, tr("QInputDialog::getItem()"), tr("Select the loader to use:"), names, 0, false, &ok);
        if (ok && !plugin_name.isEmpty())
        {
            loader = _data_loader[ plugin_name ];
            last_plugin_name_used = plugin_name;
        }
    }

    if( loader )
    {
        QFile file(filename);

        if (!file.open(QFile::ReadOnly | QFile::Text)) {
            QMessageBox::warning(this, tr("Datafile"),
                                 tr("Cannot read file %1:\n%2.")
                                 .arg(filename)
                                 .arg(file.errorString()));
            return;
        }
        file.close();

        _loaded_datafile = filename;
        ui->actionDeleteAllData->setEnabled( true );

        std::string timeindex_name_empty;
        std::string & timeindex_name = timeindex_name_empty;
        if( reuse_last_timeindex )
        {
            timeindex_name = _last_load_configuration;
        }

        PlotDataMap mapped_data = loader->readDataFromFile(
                    filename.toStdString(),
                    timeindex_name   );

        _last_load_configuration = timeindex_name;

        // remap to different type
        importPlotDataMap(mapped_data, true);
    }
    else{
        QMessageBox::warning(this, tr("Error"),
                             tr("Cannot read files with extension %1.\n No plugin can handle that!\n")
                             .arg(filename) );
    }
    _curvelist_widget->updateFilter();
}


void MainWindow::onActionReloadDataFileFromSettings()
{
    onActionLoadDataFile( true );
}

void MainWindow::onActionReloadRecentLayout()
{
    onActionLoadLayout( true );
}

void MainWindow::onActionLoadStreamer(QString streamer_name)
{
    if( _current_streamer )
    {
        _current_streamer->shutdown();
        _current_streamer = nullptr;
    }

    if( _data_streamer.empty())
    {
        qDebug() << "Error, no streamer loaded";
        return;
    }

    if( _data_streamer.size() == 1)
    {
        _current_streamer = _data_streamer.begin()->second;
    }
    else if( _data_streamer.size() > 1)
    {
        auto it = _data_streamer.find(streamer_name);
        if( it != _data_streamer.end())
        {
            _current_streamer = it->second;
        }
        else{
            qDebug() << "Error. The streamer " << streamer_name <<
                        " can't be loaded";
            return;
        }
    }

    if( _current_streamer && _current_streamer->start() )
    {
        _current_streamer->enableStreaming( false );
        importPlotDataMap( _current_streamer->getDataMap(), true );
        _loaded_datafile = QString();

        for(auto& action: ui->menuStreaming->actions()) {
            action->setEnabled(false);
        }
        ui->actionStopStreaming->setEnabled(true);
        ui->actionDeleteAllData->setEnabled( false );
        ui->actionDeleteAllData->setToolTip("Stop streaming to be able to delete the data");

        ui->pushButtonStreaming->setEnabled(true);
        ui->pushButtonStreaming->setChecked(true);

        on_streamingSpinBox_valueChanged( ui->streamingSpinBox->value() );
    }
    else{
        qDebug() << "Failed to launch the streamer";
    }
}

void MainWindow::onActionLoadLayout(bool reload_previous)
{
    QSettings settings( "IcarusTechnology", "PlotJuggler");

    QString directory_path = QDir::currentPath();

    if( settings.contains("MainWindow.lastLayoutDirectory") )
    {
        directory_path = settings.value("MainWindow.lastLayoutDirectory").toString();
    }

    QString filename;
    if( reload_previous && settings.contains("MainWindow.recentlyLoadedLayout") )
    {
        filename = settings.value("MainWindow.recentlyLoadedLayout").toString();
    }
    else{
        filename = QFileDialog::getOpenFileName(this,
                                                "Open Layout",
                                                directory_path,
                                                "*.xml");
    }
    if (filename.isEmpty())
        return;
    else
        onActionLoadLayoutFromFile(filename, true);
}

void MainWindow::onActionLoadLayoutFromFile(QString filename, bool load_data)
{
    QSettings settings( "IcarusTechnology", "PlotJuggler");

    QString directory_path = QFileInfo(filename).absolutePath();
    settings.setValue("MainWindow.lastLayoutDirectory",  directory_path);
    settings.setValue("MainWindow.recentlyLoadedLayout", filename);

    ui->actionLoadRecentLayout->setText("Load layout from: " + filename);

    QFile file(filename);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        QMessageBox::warning(this, tr("Layout"),
                             tr("Cannot read file %1:\n%2.")
                             .arg(filename)
                             .arg(file.errorString()));
        return;
    }

    QString errorStr;
    int errorLine, errorColumn;

    QDomDocument domDocument;

    if (!domDocument.setContent(&file, true, &errorStr, &errorLine, &errorColumn)) {
        QMessageBox::information(window(), tr("XML Layout"),
                                 tr("Parse error at line %1:\n%2")
                                 .arg(errorLine)
                                 .arg(errorStr));
        return;
    }

    QDomElement root = domDocument.namedItem("root").toElement();

    if(load_data)
    {
        QDomElement previously_loaded_datafile =  root.firstChildElement( "previouslyLoadedDatafile" );
        if( previously_loaded_datafile.isNull() == false)
        {
            QString filename = previously_loaded_datafile.text();

            QMessageBox::StandardButton reload_previous;
            reload_previous = QMessageBox::question(0, tr("Wait!"),
                                                    tr("Do you want to reload the datafile?\n\n[%1]\n").arg(filename),
                                                    QMessageBox::Yes | QMessageBox::No,
                                                    QMessageBox::Yes );

            if( reload_previous == QMessageBox::Yes ) {
                onActionLoadDataFileImpl( filename );
            }
        }

        QDomElement previously_loaded_streamer =  root.firstChildElement( "previouslyLoadedStreamer" );
        if( previously_loaded_streamer.isNull() == false)
        {
            QString streamer_name = previously_loaded_streamer.text();

            bool streamer_loaded = false;
            for(auto& it: _data_streamer) {
                if( it.first == streamer_name) streamer_loaded = true;
            }
            if( streamer_loaded ){
                onActionLoadStreamer( streamer_name );
            }
            else{
                QMessageBox::warning(this, tr("Error Loading Streamer"),
                                     tr("The stramer named %1 can not be loaded.").arg(streamer_name));
            }
        }
    }

    ///--------------------------------------------------

    xmlLoadState( domDocument );

    _undo_states.clear();
    _undo_states.push_back( domDocument );

    onLayoutChanged();
}


void MainWindow::onUndoInvoked( )
{
    // qDebug() << "on_UndoInvoked "<<_undo_states.size() << " -> " <<_undo_states.size()-1;

    _disable_undo_logging = true;
    if( _undo_states.size() > 1)
    {
        QDomDocument state_document = _undo_states.back();
        while( _redo_states.size() >= 100 ) _redo_states.pop_front();
        _redo_states.push_back( state_document );
        _undo_states.pop_back();
        state_document = _undo_states.back();

        xmlLoadState( state_document );

        onLayoutChanged();
    }
    _disable_undo_logging = false;
}


void MainWindow::on_tabbedAreaDestroyed(QObject *object)
{
    onLayoutChanged();
    this->setFocus();
}

void MainWindow::onFloatingWindowDestroyed(QObject *object)
{
    for (size_t i=0; i< _floating_window.size(); i++)
    {
        if( _floating_window[i] == object)
        {
            _floating_window.erase( _floating_window.begin() + i);
            break;
        }
    }
    onLayoutChanged();
}

void MainWindow::onCreateFloatingWindow(PlotMatrix* first_tab)
{
    createTabbedDialog( first_tab );
}


void MainWindow::onLayoutChanged()
{
    std::map<QString,TabbedPlotWidget*> tabbed_map;
    tabbed_map.insert( std::make_pair( QString("Main window"), _main_tabbed_widget) );

    for (SubWindow* subwin: _floating_window)
    {
        tabbed_map.insert( std::make_pair( subwin->windowTitle(), subwin->tabbedWidget() ) );
    }
    for (auto& it: tabbed_map)
    {
        it.second->setSiblingsList( tabbed_map );
    }
}

void MainWindow::forEachWidget(std::function<void (PlotWidget*, PlotMatrix*, int,int )> operation)
{
    std::vector<TabbedPlotWidget*> tabbed_plotarea;
    tabbed_plotarea.reserve( 1+ _floating_window.size());

    tabbed_plotarea.push_back( _main_tabbed_widget );
    for (SubWindow* subwin: _floating_window){
        tabbed_plotarea.push_back( subwin->tabbedWidget() );
    }

    for(size_t w=0; w < tabbed_plotarea.size(); w++)
    {
        QTabWidget * tabs = tabbed_plotarea[w]->tabWidget();

        for (int t=0; t < tabs->count(); t++)
        {
            PlotMatrix* matrix =  static_cast<PlotMatrix*>(tabs->widget(t));

            for(unsigned row=0; row< matrix->rowsCount(); row++)
            {
                for(unsigned col=0; col< matrix->colsCount(); col++)
                {
                    PlotWidget* plot = matrix->plotAt(row, col);
                    operation(plot, matrix, row, col);
                }
            }
        }
    }
}

void MainWindow::forEachWidget(std::function<void (PlotWidget *)> op)
{
    forEachWidget( [&](PlotWidget*plot, PlotMatrix*, int,int) { op(plot); } );
}

void MainWindow::updateTimeSlider()
{
    //----------------------------------
    // find min max time

    double min_time =  std::numeric_limits<double>::max();
    double max_time = -std::numeric_limits<double>::max();
    size_t max_steps = 10;

    for (auto it: _mapped_plot_data.numeric )
    {
        PlotDataPtr data = it.second;
        if(data->size() >=1)
        {
            const double t0 = data->at(0).x;
            const double t1 = data->at( data->size() -1).x;
            min_time  = std::min( min_time, t0);
            max_time  = std::max( max_time, t1);
            max_steps = std::max( max_steps, data->size());
        }
    }

    if( _mapped_plot_data.numeric.size() == 0)
    {
        min_time = 0.0;
        max_time = 0.0;
        max_steps = 1;
    }
    //----------------------------------
    // Update Time offset
    //if( update_timeoffset)
    {
        bool remove_offset = ui->pushButtonRemoveTimeOffset->isChecked();

        if( remove_offset )
        {
            if( isStreamingActive() == false){
                _time_offset.set( min_time );
            }
        }
        else{
            _time_offset.set( 0.0 );
        }
    }

    //----------------------------------
    ui->timeSlider->setLimits(min_time - _time_offset.get(),
                              max_time - _time_offset.get(),
                              max_steps);
}

void MainWindow::onSwapPlots(PlotWidget *source, PlotWidget *destination)
{
    if( !source || !destination ) return;

    PlotMatrix* src_matrix = NULL;
    PlotMatrix* dst_matrix = NULL;
    QPoint src_pos;
    QPoint dst_pos;

    forEachWidget( [&](PlotWidget* plot, PlotMatrix* matrix, int row,int col)
    {
        if( plot == source ) {
            src_matrix = matrix;
            src_pos.setX( row );
            src_pos.setY( col );
        }
        else if( plot == destination )
        {
            dst_matrix = matrix;
            dst_pos.setX( row );
            dst_pos.setY( col );
        }
    });

    if(src_matrix && dst_matrix)
    {
        src_matrix->gridLayout()->removeWidget( source );
        dst_matrix->gridLayout()->removeWidget( destination );

        src_matrix->gridLayout()->addWidget( destination, src_pos.x(), src_pos.y() );
        dst_matrix->gridLayout()->addWidget( source,      dst_pos.x(), dst_pos.y() );

        src_matrix->updateLayout();
        if( src_matrix != dst_matrix){
            dst_matrix->updateLayout();
        }
    }
    onUndoableChange();
}

void MainWindow::on_pushButtonStreaming_toggled(bool streaming)
{
    if( !_current_streamer )
    {
        streaming = false;
    }
    else{
        _current_streamer->enableStreaming( streaming ) ;
    }

    if( streaming )
    {
        ui->horizontalSpacer->changeSize(1,1, QSizePolicy::Expanding, QSizePolicy::Fixed);
        ui->pushButtonStreaming->setText("Streaming ON");
    }
    else{
        _replot_timer->stop( );
        ui->horizontalSpacer->changeSize(0,0, QSizePolicy::Fixed, QSizePolicy::Fixed);
        ui->pushButtonStreaming->setText("Streaming OFF");
    }
    ui->streamingLabel->setHidden( !streaming );
    ui->streamingSpinBox->setHidden( !streaming );
    ui->timeSlider->setHidden( streaming );

    forEachWidget( [&](PlotWidget* plot)
    {
        plot->enableTracker( !streaming );
    } );

    emit activateStreamingMode( streaming );

    this->repaint();

    if( _current_streamer && streaming)
    {
        _replot_timer->setSingleShot(true);
        _replot_timer->start( 5 );

        double min_time = std::numeric_limits<double>::max();
        for (auto it: _mapped_plot_data.numeric )
        {
            PlotDataPtr data = it.second;
            data->flushAsyncBuffer();
            if(data->size() > 0)
            {
                min_time  = std::min( min_time,  data->at(0).x);
            }
        }

        if( min_time == std::numeric_limits<double>::max())
        {
            using namespace std::chrono;
            auto epoch = high_resolution_clock::now().time_since_epoch();
            min_time = duration<double>(epoch).count();
        }
        _time_offset.set(min_time);
    }
    else{
        onUndoableChange();
    }
}

void MainWindow::updateDataAndReplot()
{

    // STEP 1: sync the data (usefull for streaming
    bool data_updated = false;
    {
        PlotData::asyncPushMutex().lock();
        for(auto it : _mapped_plot_data.numeric)
        {
            PlotDataPtr data = ( it.second );
            data_updated |=  data->flushAsyncBuffer();
        }
        PlotData::asyncPushMutex().unlock();
    }

    if( data_updated )
    {
        forEachWidget( [](PlotWidget* plot)
        {
            plot->updateCurves(true);
        } );
        updateTimeSlider();
    }
    //--------------------------------
    // trigger again the execution of this callback if steaming == true
    if( isStreamingActive())
    {
        static auto prev_time = std::chrono::steady_clock::now();
        auto time_now =  std::chrono::steady_clock::now();
        if( (time_now - prev_time) > std::chrono::seconds(2) )
        {
            prev_time = time_now;
            importPlotDataMap( _current_streamer->getDataMap(), false );
        }

        _replot_timer->setSingleShot(true);
        _replot_timer->stop( );
        _replot_timer->start( 40 ); // 25 Hz at most

        _tracker_time = ui->timeSlider->getMaximum() + _time_offset.get();
        forEachWidget( [&](PlotWidget* plot)
        {
            plot->setTrackerPosition( _tracker_time );
        } );

        onTrackerTimeUpdated(_tracker_time);
    }
    //--------------------------------
    // zoom out and replot
    _main_tabbed_widget->currentTab()->maximumZoomOut() ;

    for(SubWindow* subwin: _floating_window)
    {
        PlotMatrix* matrix =  subwin->tabbedWidget()->currentTab() ;
        matrix->maximumZoomOut(); // includes replot
    }
}

void MainWindow::on_streamingSpinBox_valueChanged(int value)
{
    for (auto it : _mapped_plot_data.numeric )
    {
        PlotDataPtr plot = it.second;
        plot->setMaximumRangeX( value );
    }

    for (auto it: _mapped_plot_data.user_defined)
    {
        PlotDataAnyPtr plot = it.second;
        plot->setMaximumRangeX( value );
    }
}

void MainWindow::on_actionAbout_triggered()
{
    AboutDialog* aboutdialog = new AboutDialog(this);
    aboutdialog->show();
}

void MainWindow::on_actionStopStreaming_triggered()
{
    ui->pushButtonStreaming->setChecked(false);
    ui->pushButtonStreaming->setEnabled(false);
    _current_streamer->shutdown();
    _current_streamer = nullptr;

    for(auto& action: ui->menuStreaming->actions()) {
        action->setEnabled(true);
    }
    ui->actionStopStreaming->setEnabled(false);

    if( !_mapped_plot_data.numeric.empty()){
        ui->actionDeleteAllData->setEnabled( true );
        ui->actionDeleteAllData->setToolTip("");
    }
}


void MainWindow::on_actionExit_triggered()
{
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(0, tr("Warning"),
                                  tr("Do you really want quit?\n"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::Yes );
    if( reply == QMessageBox::Yes ) {
        this->close();
    }
}

void MainWindow::on_actionQuick_Help_triggered()
{
    const QString path =  tr(PJ_DOCUMENTATION_DIR) + "/index.html";
    QFileInfo check_file(path);

    QUrl url_SYS( tr("file:///")  + path, QUrl::TolerantMode);

    if( check_file.exists() && url_SYS.isValid() )
    {
        if(QDesktopServices::openUrl(url_SYS))
        {
            return;
        }
    }
    QMessageBox::warning(this, "Can't find Documentation",
                         QString("Can't open the file:\n\n%1\n\n Is it correctly installed in your system?").arg( url_SYS.path()) );
}


void MainWindow::on_pushButtonRemoveTimeOffset_toggled(bool )
{
    updateTimeSlider();
    updatedDisplayTime();
    if (this->signalsBlocked() == false)  onUndoableChange();
}


void MainWindow::on_pushButtonOptions_toggled(bool checked)
{
    ui->widgetOptions->setVisible( checked );
    ui->line->setVisible( checked );
}

void MainWindow::updatedDisplayTime()
{
    const double relative_time = _tracker_time - _time_offset.get();
    if( ui->pushButtonUseDateTime->isChecked() )
    {
        if( _time_offset.get() > 0 )
        {
            QTime time = QTime::fromMSecsSinceStartOfDay( std::round(relative_time*1000.0));
            ui->displayTime->setText( time.toString("HH:mm::ss.zzz") );
        }
        else{
            QDateTime datetime = QDateTime::fromMSecsSinceEpoch( std::round(relative_time*1000.0) );
            ui->displayTime->setText( datetime.toString("d/M/yy HH:mm::ss.zzz") );
        }
    }
    else{
        ui->displayTime->setText( QString::number(relative_time, 'f', 3));
    }
}

void MainWindow::on_pushButtonActivateGrid_toggled(bool checked)
{
    forEachWidget( [checked](PlotWidget* plot) {
        plot->activateGrid( checked );
        plot->replot();
    });
}

void MainWindow::on_actionClearBuffer_triggered()
{
    {
        std::unique_lock<std::mutex> locker( PlotData::asyncPushMutex() );
        for (auto it: _mapped_plot_data.numeric )
        {
            it.second->clear();
        }
    }

    {
        std::unique_lock<std::mutex> locker( PlotDataAny::asyncPushMutex() );
        for (auto it: _mapped_plot_data.user_defined )
        {
            it.second->clear();
        }
    }
    forEachWidget( [](PlotWidget* plot) {
        plot->reloadPlotData();
        plot->replot();
    });
}

void MainWindow::on_pushButtonUseDateTime_toggled(bool checked)
{
    updatedDisplayTime();
}

void MainWindow::on_pushButtonTimeTracker_pressed()
{
    if( _tracker_param == CurveTracker::LINE_ONLY)
    {
        _tracker_param = CurveTracker::VALUE;
    }
    else if( _tracker_param == CurveTracker::VALUE)
    {
        _tracker_param = CurveTracker::VALUE_NAME;
    }
    else if( _tracker_param == CurveTracker::VALUE_NAME)
    {
        _tracker_param = CurveTracker::LINE_ONLY;
    }
    ui->pushButtonTimeTracker->setIcon( _tracker_button_icons[ _tracker_param ] );

    forEachWidget( [&](PlotWidget* plot) {
        plot->configureTracker(_tracker_param);
        plot->replot();
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if( _current_streamer )
    {
        _current_streamer->shutdown();
        _current_streamer = nullptr;
    }
    QSettings settings( "IcarusTechnology", "PlotJuggler");
    settings.setValue("MainWindow.geometry", saveGeometry());
    settings.setValue("MainWindow.activateGrid", ui->pushButtonActivateGrid->isChecked() );
    settings.setValue("MainWindow.streamingBufferValue", ui->streamingSpinBox->value() );
    settings.setValue("MainWindow.dateTimeDisplay",ui->pushButtonUseDateTime->isChecked() );
    settings.setValue("MainWindow.timeTrackerSetting", (int)_tracker_param );
}
