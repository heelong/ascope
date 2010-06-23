#include "AScope.h"
#include "ScopePlot.h"
#include "Knob.h"
//#include "SvnVersion.h"

#include <QMessageBox>
#include <QButtonGroup>
#include <QLabel>
#include <QTimer>
#include <QSpinBox>
#include <QLCDNumber>
#include <QSlider>
#include <QLayout>
#include <QTabWidget>
#include <QWidget>
#include <QRadioButton>
#include <QButtonGroup>
#include <QFrame>
#include <QPushButton>
#include <QPalette>
#include <QDateTime>
#include <QFileDialog>
#include <QProgressBar>
#include <string>
#include <algorithm>

#include <iostream>
#include <time.h>

#include <qwt_wheel.h>

//////////////////////////////////////////////////////////////////////
AScope::AScope(
        QWidget* parent) :
    QWidget(parent),
    _statsUpdateInterval(5),
            _timeSeriesPlot(TRUE), _config("NCAR", "AScope"),
            _paused(false), _zeroMoment(0.0),
            _channel(0), _gateChoice(0),
            _combosInitialized(false){
    // Set up our form
    setupUi(this);

    // get our title from the coniguration
    std::string title = _config.getString("title", "AScope");
    title += " ";
    //title += SvnVersion::revision();
    //QApplication::activeWindow()->setWindowTitle(title.c_str());

    // initialize running statistics
    for (int i = 0; i < 3; i++) {
        //		_pulseCount[i]	= 0;
        //		_prevPulseCount[i] = 0;
        _errorCount[i] = 0;
        _lastPulseNum[i] = 0;
    }

	// create a button group for the channels
	_chanButtonGroup = new QButtonGroup;

    // connect the controls
    connect(_autoScale, SIGNAL(released()), this, SLOT(autoScaleSlot()));
    connect(_gainKnob, SIGNAL(valueChanged(double)), this, SLOT(gainChangeSlot(double)));
    connect(_up, SIGNAL(released()), this, SLOT(upSlot()));
    connect(_dn, SIGNAL(released()), this, SLOT(dnSlot()));
    connect(_saveImage, SIGNAL(released()), this, SLOT(saveImageSlot()));
    connect(_pauseButton, SIGNAL(toggled(bool)), this, SLOT(pauseSlot(bool)));
    connect(_windowButton, SIGNAL(toggled(bool)), this, SLOT(windowSlot(bool)));
    connect(_gateNumber, SIGNAL(activated(int)), this, SLOT(gateChoiceSlot(int)));
    connect(_xGrid, SIGNAL(toggled(bool)), _scopePlot, SLOT(enableXgrid(bool)));
    connect(_yGrid, SIGNAL(toggled(bool)), _scopePlot, SLOT(enableYgrid(bool)));
    connect(_blockSizeCombo, SIGNAL(activated(int)), this, SLOT(blockSizeSlot(int)));
    connect(_chanButtonGroup, SIGNAL(buttonReleased(int)), this, SLOT(channelSlot(int)));

    // set the checkbox selections
    _pauseButton->setChecked(false);
    _xGrid->setChecked(true);
    _yGrid->setChecked(true);

    // initialize the book keeping for the plots.
    // This also sets up the radio buttons
    // in the plot type tab widget
    initPlots();

    _gainKnob->setRange(-7, 7);
    _gainKnob->setTitle("Gain");

    // set the minor ticks
    _gainKnob->setScaleMaxMajor(5);
    _gainKnob->setScaleMaxMinor(5);
    
    // initialize the activity bar
    _activityBar->setRange(0, 100);
    _activityBar->setValue(0);

    _xyGraphRange = 1;
    _xyGraphCenter = 0.0;
    _knobGain = 0.0;
    _knobOffset = 0.0;
    _specGraphRange = 120.0;
    _specGraphCenter = -40.0;

    // set up the palettes
    _greenPalette = this->palette();
    _greenPalette.setColor(this->backgroundRole(), QColor("green"));
    _redPalette = _greenPalette;
    _redPalette.setColor(this->backgroundRole(), QColor("red"));

    // The initial plot type will be I and Q timeseries
    plotTypeSlot(TS_TIMESERIES_PLOT);


    // start the statistics timer
    startTimer(_statsUpdateInterval*1000);

    // let the data sources get themselves ready
    sleep(1);

}
//////////////////////////////////////////////////////////////////////
AScope::~AScope() {
}

//////////////////////////////////////////////////////////////////////
void AScope::initCombos(int channels, int tsLength, int gates) {
	// initialize the fft numerics
	initFFT(tsLength);

	// initialize the number of gates.
	initGates(gates);

	// initialize the channels
	initChans(channels);


}
//////////////////////////////////////////////////////////////////////
void AScope::initGates(int gates) {
	// populate the gate selection combo box
	for (int g = 0; g < gates; g++) {
        QString l = QString("%1").arg(g);
		_gateNumber->addItem(l, QVariant(g));
	}
}

//////////////////////////////////////////////////////////////////////
void AScope::initChans(int channels) {

    // create the channel seletion radio buttons.

	QVBoxLayout *vbox = new QVBoxLayout;
	_chanBox->setLayout(vbox);

	for (int c = 0; c < channels; c++) {
		// create the button and add to the layout
		QString l = QString("Chan %1").arg(c);
		QRadioButton* r = new QRadioButton(l);
		vbox->addWidget(r);
		// add it to the button group, with the channel
		// number as the id
		_chanButtonGroup->addButton(r, c);
		// select the first button
		if (c == 0) {
			r->setChecked(true);
		    _channel = 0;
		} else {
			r->setChecked(false);
		}
	}
}
//////////////////////////////////////////////////////////////////////
void AScope::initFFT(int tsLength) {

    // configure the block/fft size selection
    /// @todo add logic to insure that smallest fft size is a power of two.
    int fftSize = 8;
    int maxFftSize = (int) pow(2, floor(log2(tsLength)));
    for (; fftSize <= maxFftSize; fftSize = fftSize*2) {
        _blockSizeChoices.push_back(fftSize);
        QString l = QString("%1").arg(fftSize);
        _blockSizeCombo->addItem(l, QVariant(fftSize));
    }

    // select the last choice for the block size
    _blockSizeIndex = (_blockSizeChoices.size()-1);
    _blockSizeCombo->setCurrentIndex(_blockSizeIndex);

    //  set up fft for power calculations:
    _fftwData.resize(_blockSizeChoices.size());
    _fftwPlan.resize(_blockSizeChoices.size());
    for (unsigned int i = 0; i < _blockSizeChoices.size(); i++) {
        // allocate the data space for fftw
        int blockSize = _blockSizeChoices[i];
        _fftwData[i] = (fftw_complex*)fftw_malloc(sizeof(fftw_complex)
                * blockSize);
        // create the plan.
        _fftwPlan[i] = fftw_plan_dft_1d(blockSize, _fftwData[i], _fftwData[i],
        FFTW_FORWARD,
        FFTW_ESTIMATE);
    }

    // create the hamming coefficients
    hammingSetup();
}

//////////////////////////////////////////////////////////////////////
void AScope::saveImageSlot() {
    QString f = _config.getString("imageSaveDirectory", "c:/").c_str();

    QFileDialog d( this, tr("Save AScope Image"), f,
            tr("PNG files (*.png);;All files (*.*)"));
    d.setFileMode(QFileDialog::AnyFile);
    d.setViewMode(QFileDialog::Detail);
    d.setAcceptMode(QFileDialog::AcceptSave);
    d.setConfirmOverwrite(true);
    d.setDefaultSuffix("png");
    d.setDirectory(f);

    f = "AScope-";
    f += QDateTime::currentDateTime().toString("yyyy-MM-dd-hh-mm-ss");
    f += ".png";
    d.selectFile(f);
    if (d.exec()) {
        QStringList saveNames = d.selectedFiles();
        _scopePlot->saveImageToFile(saveNames[0].toStdString());
        f = d.directory().absolutePath();
        _config.setString("imageSaveDirectory", f.toStdString());
    }
}
//////////////////////////////////////////////////////////////////////
void AScope::processTimeSeries(
        std::vector<double>& Idata, std::vector<double>& Qdata) {
    if (!_timeSeriesPlot)
        return;

    PlotInfo* pi = &_tsPlotInfo[_tsPlotType];
    switch (pi->getDisplayType()) {
    case ScopePlot::SPECTRUM: {

        // compute the power spectrum
        _zeroMoment = powerSpectrum(Idata, Qdata);
        displayData();
        break;
    }
    case SCOPE_PLOT_TIMESERIES:
    case SCOPE_PLOT_IVSQ: {
        I.resize(Idata.size());
        Q.resize(Qdata.size());
        I = Idata;
        Q = Qdata;
        _zeroMoment = zeroMomentFromTimeSeries(I, Q);
        displayData();
        break;
    }
    default:
        // ignore others
        break;
    }
}

//////////////////////////////////////////////////////////////////////
void AScope::displayData() {
    double yBottom = _xyGraphCenter - _xyGraphRange;
    double yTop = _xyGraphCenter + _xyGraphRange;

    QString l = QString("%1").arg(_zeroMoment, 6, 'f', 1);
    _powerDB->setText(l);

    // Time series data display
    PlotInfo* pi = &_tsPlotInfo[_tsPlotType];

    std::string xlabel;
    ScopePlot::PLOTTYPE displayType =
            (ScopePlot::PLOTTYPE) pi->getDisplayType();
    switch (displayType) {
    case ScopePlot::TIMESERIES:
        if (pi->autoscale()) {
            autoScale(I, Q, displayType);
            pi->autoscale(false);
        }
        xlabel = std::string("Time");
        _scopePlot->TimeSeries(I, Q, yBottom, yTop, 1, xlabel, "I - Q");
        break;
    case ScopePlot::IVSQ:
        if (pi->autoscale()) {
            autoScale(I, Q, displayType);
            pi->autoscale(false);
        }
        _scopePlot->IvsQ(I, Q, yBottom, yTop, 1, "I", "Q");
        break;
    case ScopePlot::SPECTRUM:
        if (pi->autoscale()) {
            autoScale(_spectrum, displayType);
            pi->autoscale(false);
        }
        _scopePlot->Spectrum(_spectrum, _specGraphCenter-_specGraphRange
                /2.0, _specGraphCenter+_specGraphRange/2.0, 1000000, false,
                "Frequency (Hz)", "Power (dB)");
        break;
    case ScopePlot::PRODUCT:
        // include just to quiet compiler warnings
        break;
    }
}

//////////////////////////////////////////////////////////////////////
double AScope::powerSpectrum(
        std::vector<double>& Idata, std::vector<double>& Qdata) {

    int blockSize = _blockSizeChoices[_blockSizeIndex];

    _spectrum.resize(blockSize);
    int n = Idata.size();
    if (blockSize < n) {
        n = blockSize;
    }
    for (int j = 0; j < n; j++) {
        // transfer the data to the fftw input space
        _fftwData[_blockSizeIndex][j][0] = Idata[j];
        _fftwData[_blockSizeIndex][j][1] = Qdata[j];
    }
    // zero pad, if we are looking at along beam data.
    for (int j = n; j < blockSize; j++) {
        _fftwData[_blockSizeIndex][j][0] = 0;
        _fftwData[_blockSizeIndex][j][1] = 0;
    }

    // apply the hamming window to the time series
    if (_doHamming)
        doHamming();

    // caclulate the fft
    fftw_execute(_fftwPlan[_blockSizeIndex]);

    double zeroMoment = 0.0;

    // reorder and copy the results into _spectrum
    for (int i = 0; i < blockSize/2; i++) {
        double pow = _fftwData[_blockSizeIndex][i][0]
                * _fftwData[_blockSizeIndex][i][0]
                + _fftwData[_blockSizeIndex][i][1]
                        * _fftwData[_blockSizeIndex][i][1];

        zeroMoment += pow;

        pow /= blockSize*blockSize;
        pow = 10.0*log10(pow);
        _spectrum[i+blockSize/2] = pow;
    }

    for (int i = blockSize/2; i < blockSize; i++) {
        double pow = _fftwData[_blockSizeIndex][i][0]
                * _fftwData[_blockSizeIndex][i][0]
                + _fftwData[_blockSizeIndex][i][1]
                        * _fftwData[_blockSizeIndex][i][1];

        zeroMoment += pow;

        pow /= blockSize*blockSize;
        pow = 10.0*log10(pow);
        _spectrum[i - blockSize/2] = pow;
    }

    zeroMoment /= blockSize*blockSize;
    zeroMoment = 10.0*log10(zeroMoment);

    return zeroMoment;
}

////////////////////////////////////////////////////////////////////
void AScope::plotTypeSlot(
        int plotType) {

    // find out the index of the current page
    int pageNum = _typeTab->currentIndex();

    // get the radio button id of the currently selected button
    // on that page.
    int ptype = _tabButtonGroups[pageNum]->checkedId();

    // change to a raw plot type
    TS_PLOT_TYPES tstype = (TS_PLOT_TYPES)ptype;
    plotTypeChange( &_tsPlotInfo[tstype], tstype);
}

//////////////////////////////////////////////////////////////////////
void AScope::tabChangeSlot(
        QWidget* w) {
    // find out the index of the current page
    int pageNum = _typeTab->currentIndex();

    // get the radio button id of the currently selected button
    // on that page.
    int ptype = _tabButtonGroups[pageNum]->checkedId();

    // change to a raw plot type
    TS_PLOT_TYPES plotType = (TS_PLOT_TYPES)ptype;
    plotTypeChange( &_tsPlotInfo[plotType], plotType);
}

////////////////////////////////////////////////////////////////////
void AScope::plotTypeChange(
        PlotInfo* pi, TS_PLOT_TYPES newPlotType) {

    // save the gain and offset of the current plot type
    PlotInfo* currentPi;
    currentPi = &_tsPlotInfo[_tsPlotType];
    currentPi->setGain(pi->getGainMin(), pi->getGainMax(), _knobGain);
    currentPi->setOffset(pi->getOffsetMin(), pi->getOffsetMax(), _xyGraphCenter);

    // restore gain and offset for new plot type
    gainChangeSlot(pi->getGainCurrent());
    _xyGraphCenter = pi->getOffsetCurrent();

    // set the knobs for the new plot type
    _gainKnob->setValue(_knobGain);

     _tsPlotType = newPlotType;

}

////////////////////////////////////////////////////////////////////
void AScope::initPlots() {

    _pulsePlots.insert(TS_TIMESERIES_PLOT);
    _pulsePlots.insert(TS_IVSQ_PLOT);
    _pulsePlots.insert(TS_SPECTRUM_PLOT);

    _tsPlotInfo[TS_TIMESERIES_PLOT] = PlotInfo(TS_TIMESERIES_PLOT,
            SCOPE_PLOT_TIMESERIES, "I and Q", "S:  I and Q", -5.0, 5.0, 0.0,
            -5.0, 5.0, 0.0);
    _tsPlotInfo[TS_IVSQ_PLOT] = PlotInfo(TS_IVSQ_PLOT, SCOPE_PLOT_IVSQ,
            "I vs Q", "S:  I vs Q", -5.0, 5.0, 0.0, -5.0, 5.0, 0.0);
    _tsPlotInfo[TS_SPECTRUM_PLOT] = PlotInfo(TS_SPECTRUM_PLOT,
            SCOPE_PLOT_SPECTRUM, "Power Spectrum", "S:  Power Spectrum", -5.0,
            5.0, 0.0, -5.0, 5.0, 0.0);

    // remove the one tab that was put there by designer
    _typeTab->removeTab(0);

    // add tabs, and save the button group for
    // for each tab.
    QButtonGroup* pGroup;

    pGroup = addTSTypeTab("I & Q", _pulsePlots);
    _tabButtonGroups.push_back(pGroup);

    connect(_typeTab, SIGNAL(currentChanged(QWidget *)),
            this, SLOT(tabChangeSlot(QWidget*)));
}

//////////////////////////////////////////////////////////////////////
QButtonGroup* AScope::addTSTypeTab(
        std::string tabName, std::set<TS_PLOT_TYPES> types) {
    // The page that will be added to the tab widget
    QWidget* pPage = new QWidget;
    // the layout manager for the page, will contain the buttons
    QVBoxLayout* pVbox = new QVBoxLayout;
    // the button group manager, which has nothing to do with rendering
    QButtonGroup* pGroup = new QButtonGroup;

    std::set<TS_PLOT_TYPES>::iterator i;

    for (i = types.begin(); i != types.end(); i++) {
        // create the radio button
        int id = _tsPlotInfo[*i].getId();
        QRadioButton* pRadio = new QRadioButton;
        const QString label = _tsPlotInfo[*i].getLongName().c_str();
        pRadio->setText(label);

        // put the button in the button group
        pGroup->addButton(pRadio, id);
        // assign the button to the layout manager
        pVbox->addWidget(pRadio);

        // set the first radio button of the group
        // to be selected.
        if (i == types.begin()) {
            pRadio->setChecked(true);
        }
    }
    // associate the layout manager with the page
    pPage->setLayout(pVbox);

    // put the page on the tab
    _typeTab->insertTab(-1, pPage, tabName.c_str());

    // connect the button released signal to our plot type change slot.
    connect(pGroup, SIGNAL(buttonReleased(int)), this, SLOT(plotTypeSlot(int)));

    return pGroup;
}

//////////////////////////////////////////////////////////////////////
void AScope::timerEvent(
        QTimerEvent*) {

}

//////////////////////////////////////////////////////////////////////
void AScope::gainChangeSlot(
        double gain) {

    // keep a local copy of the gain knob value
    _knobGain = gain;

    _specGraphRange = pow(10.0, gain+2.0);

    _xyGraphRange = pow(10.0, -gain);

    _gainKnob->setValue(gain);

}

//////////////////////////////////////////////////////////////////////
void AScope::upSlot() {
    bool spectrum = false;

    if (_timeSeriesPlot) {
        PlotInfo* pi = &_tsPlotInfo[_tsPlotType];
        if (pi->getDisplayType() == ScopePlot::SPECTRUM) {
            spectrum = true;
        }
    }

    if (!spectrum) {
        _xyGraphCenter -= 0.03*_xyGraphRange;
    } else {
        _specGraphCenter -= 0.03*_specGraphRange;
    }
    displayData();
}

//////////////////////////////////////////////////////////////////////
void AScope::dnSlot() {

    bool spectrum = false;

    if (_timeSeriesPlot) {
        PlotInfo* pi = &_tsPlotInfo[_tsPlotType];
        if (pi->getDisplayType() == ScopePlot::SPECTRUM) {
            spectrum = true;
        }
    }

    if (!spectrum) {
        _xyGraphCenter += 0.03*_xyGraphRange;
    } else {
        _specGraphCenter += 0.03*_specGraphRange;
    }

    displayData();
}

//////////////////////////////////////////////////////////////////////
void AScope::autoScale(
        std::vector<double>& data, ScopePlot::PLOTTYPE displayType) {
    if (data.size() == 0)
        return;

    // find the min and max
    double min = *std::min_element(data.begin(), data.end());
    double max = *std::max_element(data.begin(), data.end());

    // adjust the gains
    adjustGainOffset(min, max, displayType);
}

//////////////////////////////////////////////////////////////////////
void AScope::autoScale(
        std::vector<double>& data1, std::vector<double>& data2,
        ScopePlot::PLOTTYPE displayType) {
    if (data1.size() == 0 || data2.size() == 0)
        return;

    // find the min and max
    double min1 = *std::min_element(data1.begin(), data1.end());
    double min2 = *std::min_element(data2.begin(), data2.end());
    double min = std::min(min1, min2);

    double max1 = *std::max_element(data1.begin(), data1.end());
    double max2 = *std::max_element(data2.begin(), data2.end());
    double max = std::max(max1, max2);

    // adjust the gains
    adjustGainOffset(min, max, displayType);

}

//////////////////////////////////////////////////////////////////////
void AScope::adjustGainOffset(
        double min, double max, ScopePlot::PLOTTYPE displayType) {
    if (displayType == ScopePlot::SPECTRUM) {
        // currently in spectrum plot mode
        _specGraphCenter = min + (max-min)/2.0;
        _specGraphRange = 3*(max-min);
        _knobGain = -log10(_specGraphRange);
    } else {
        double factor = 0.8;
        _xyGraphCenter = (min+max)/2.0;
        _xyGraphRange = (1/factor)*(max - min)/2.0;
        if (min == max ||
        		isnan(min) ||
        		isnan(max) ||
        		isinf(min) ||
        		isinf(max))
        	_xyGraphRange = 1.0;
        //std::cout << "min:"<<min<<"  max:"<<max<<"     _xxGraphRange is " << _xyGraphRange << "\n";
        _knobGain = -log10(_xyGraphRange);
        _gainKnob->setValue(_knobGain);
    }
}


//////////////////////////////////////////////////////////////////////
void
AScope::newTSItemSlot(AScope::TimeSeries pItem) {

	int chanId = pItem.chanId;
	int tsLength = pItem.tsLength;
	// Get the gate count from the first sample
    int gates = pItem.gates;

	if (!_combosInitialized) {
		initCombos(4, tsLength, gates);
		_combosInitialized = true;
	}

	if (chanId == _channel && !_paused) {
		int blockSize = _blockSizeChoices[_blockSizeIndex];
		std::vector<double> I, Q;
		I.resize(blockSize);
		Q.resize(blockSize);

		// extract the time series from the DDS sample
		for (int t = 0; t < blockSize; t++) {
            const short* ts = pItem.data + 2 * gates;
			I[t] = ts[_gateChoice * 2];
			Q[t] = ts[_gateChoice * 2 + 1];
		}

		// process the time series
		processTimeSeries(I, Q);
	}

	// return the DDS item
	emit returnTSItem(pItem);
	
	// bump the activity bar
	_activityBar->setValue((_activityBar->value()+1) % 100);
}

//////////////////////////////////////////////////////////////////////
void AScope::autoScaleSlot() {
    PlotInfo* pi;

    pi = &_tsPlotInfo[_tsPlotType];

    pi->autoscale(true);
}

//////////////////////////////////////////////////////////////////////
void AScope::pauseSlot(
        bool p) {
    _paused = p;
}

//////////////////////////////////////////////////////////////////////
void AScope::channelSlot(
        int c) {
    _channel = c;
}

//////////////////////////////////////////////////////////////////////
void AScope::gateChoiceSlot(
        int index) {
    _gateChoice = index;
}

//////////////////////////////////////////////////////////////////////
void AScope::blockSizeSlot(
        int index) {

	_blockSizeIndex = index;

    // recalculate the hamming coefficients. _blockSizeIndex
	// must be set correctly before calling this
    hammingSetup();

}

////////////////////////////////////////////////////////////////////////

double AScope::zeroMomentFromTimeSeries(
        std::vector<double>& I, std::vector<double>& Q) {
    double p = 0;
    int n = I.size();

    for (unsigned int i = 0; i < I.size(); i++) {
        p += I[i]*I[i] + Q[i]*Q[i];
    }

    p /= n;
    p = 10.0*log10(p);
    return p;
}

////////////////////////////////////////////////////////////////////////
void
AScope::doHamming() {

  int blockSize = _blockSizeChoices[_blockSizeIndex];

  for (int i = 0; i < blockSize; i++) {
    _fftwData[_blockSizeIndex][i][0] *= _hammingCoefs[i];
    _fftwData[_blockSizeIndex][i][1] *= _hammingCoefs[i];
  }
}
////////////////////////////////////////////////////////////////////////

void
AScope::hammingSetup() {

   int blockSize = _blockSizeChoices[_blockSizeIndex];

  _hammingCoefs.resize(blockSize);

  for (int i = 0; i < blockSize; i++) {
    _hammingCoefs[i] = 0.54 - 0.46*(cos(2.0*M_PI*i/(blockSize-1)));
  }

}

////////////////////////////////////////////////////////////////////////

void
AScope::windowSlot(bool flag) {
	_doHamming = flag;
}