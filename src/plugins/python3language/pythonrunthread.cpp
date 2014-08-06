#include "pythonrunthread.h"
#include "interpretercallback.h"
#include "actorshandler.h"
#include "pyutils.h"
#include "interfaces/runinterface.h"
#include "variablesmodel.h"

extern "C" {
#include <Python.h>
}

namespace Python3Language {

using namespace Shared;

PythonRunThread* PythonRunThread::self = 0;

PythonRunThread* PythonRunThread::instance(QObject *parent, const QString &extraPythonPath)
{
    if (!self) {
        self = new PythonRunThread(parent, extraPythonPath);
    }
    return self;
}

PythonRunThread::PythonRunThread(QObject *parent, const QString & extraPythonPath)
    : QThread(parent)
    , callback_(InterpreterCallback::instance(this))
    , actorsHandler_(ActorsHandler::instance(this))
    , pythonPath_(extraPythonPath)
    , mutex_(new QMutex)
    , testingMode_(false)
    , runPauseSemaphore_(new QSemaphore(0))
    , runInputSemaphore_(new QSemaphore(0))
    , variablesModel_(new VariablesModel(this))
    , canStepOut_(false)
{
    connect(callback_, SIGNAL(errorMessageRequest(QString)),
            this, SIGNAL(errorOutputRequest(QString)),
            Qt::DirectConnection);
    connect(callback_, SIGNAL(outputMessageRequest(QString)),
            this, SIGNAL(outputRequest(QString)),
            Qt::DirectConnection);
    connect(callback_, SIGNAL(inputRequest()),
            this, SLOT(handlePythonInput()),
            Qt::DirectConnection);

    reset();
}

void PythonRunThread::reset()
{
    QMutexLocker l(mutex_);
    lineNumber_ = -1;
    errorText_.clear();
    testingResult_.clear();
    stepsCounted_ = 0;
    justStarted_ = true;
    stopping_ = false;
    releaseSemaphores();
    runPauseSemaphore_->acquire();
    runInputSemaphore_->acquire();
    canStepOut_ = false;
}

void PythonRunThread::releaseSemaphores()
{
    if (!runPauseSemaphore_->available()) {
        runPauseSemaphore_->release();
    }

    if (!runInputSemaphore_->available()) {
        runInputSemaphore_->release();
    }
}

void PythonRunThread::run()
{
    // Clear states
    reset();
    callback_->reset();
    actorsHandler_->reset();
    variablesModel_->resetModel();

    // Initialize interpreter in current thread
    PyGILState_Ensure();
    PyThreadState * py = Py_NewInterpreter();
    appendToSysPath(pythonPath_);   
    PyObject* py_run_wrapper = PyImport_ImportModule("run_wrapper");
    if (!py_run_wrapper) { printPythonTraceback(); return; }    
    PyEval_ReleaseThread(py);

    // Create actor 'modules'
    clearCreatedModules();
    for (int i=0; i<actorsHandler_->size(); i++) {
        const QString source = actorsHandler_->moduleWrapper(i);
        const QString name = actorsHandler_->moduleName(i);
        createModuleFromSource(py, name, source);
    }

    // Prepare pre-run and post-run program code
    mutex_->lock();
    PyEval_AcquireThread(py);
    PyObject * preCode = 0;
    if (preRunSource_.trimmed().length() > 0)
        preCode = compileModule(
                QString("<hidden>"),
                preRunSource_,
                &lineNumber_,
                &errorText_
                );
    PyObject * postCode = 0;
    if (postRunSource_.trimmed().length() > 0)
        postCode = compileModule(
                QString("<hidden>"),
                postRunSource_,
                &lineNumber_,
                &errorText_
                );
    PyEval_ReleaseThread(py);
    mutex_->unlock();

    // Prepare main program code
    mutex_->lock();
    PyEval_AcquireThread(py);
    PyObject * code = compileModule(
                sourceProgramPath_.isEmpty()
                ? QString("<program>") : sourceProgramPath_,
                sourceProgram_,
                &lineNumber_,
                &errorText_
                );
    PyEval_ReleaseThread(py);
    bool testingMode = testingMode_;
    mutex_->unlock();

    if (!code) {
        // There is fatal syntax error in program
        Q_EMIT stopped(RunInterface::SR_Error);
    }
    else {
        PyEval_AcquireThread(py);
        // Set interpreter tracing
        PyEval_SetTrace(&python_trace_dispatch, 0);

        // Create main module
        // NOTE in this stage name must be '__main__' to ensure builtins available
        PyObject *mainModule = PyImport_AddModule("__main__");

        // Prepare globals
        PyObject *globals = PyModule_GetDict(mainModule);
        if (testingMode) {
            PyDict_SetItemString(globals, "__name__", QStringToPyUnicode("__testing__"));
        }

        // In tesing mode call __pre_run__ if present
        if (testingMode && preCode) {
            PyEval_EvalCode(preCode, globals, globals);
            PyObject * emptyArgs = PyTuple_New(0);
            PyObject * emptyKwds = PyDict_New();
            PyObject * __pre_run__ = PyObject_GetAttrString(mainModule, "__pre_run__");
            PyObject_Call(__pre_run__, emptyArgs, emptyKwds);
        }

        // Evaluate
        PyEval_EvalCode(code, globals, globals);

        // In tesing mode call __post_run__ if present
        if (testingMode && postCode) {
            PyEval_EvalCode(postCode, globals, globals);
            PyObject * __post_run__ = PyObject_GetAttrString(mainModule, "__post_run__");
            PyObject * emptyArgs = PyTuple_New(0);
            PyObject * emptyKwds = PyDict_New();
            PyObject * pyTestResult = PyObject_Call(__post_run__, emptyArgs, emptyKwds);
            if (pyTestResult && PyLong_Check(pyTestResult)) {
                int testingResult = PyLong_AsLong(pyTestResult);
                mutex_->lock();
                testingResult_ = QVariant(testingResult);
                qDebug() << "Testing result = " << testingResult;
                mutex_->unlock();
            }
        }

        // Unset interpreter tracing
        PyEval_SetTrace(0 ,0);
        PyEval_ReleaseThread(py);
    }

    // Finalize interpreter
    PyEval_AcquireThread(py);
    Py_EndInterpreter(py);
    py = 0;
    PyEval_ReleaseLock();

    Q_EMIT updateStepsCounter(stepsCounted_);

    RunInterface::StopReason exitStatus = RunInterface::SR_Done;
    mutex_->lock();
    if (stopping_)
        exitStatus = RunInterface::SR_UserTerminated;
    else if (errorText_.length() > 0)
        exitStatus = RunInterface::SR_Error;
    mutex_->unlock();
    Q_EMIT stopped(exitStatus);
    setTestingMode(false);
}

int PythonRunThread::python_trace_dispatch(PyObject *, PyFrameObject *frame, int what, PyObject *arg)
{
    const QString fileName = PyUnicodeToQString(frame->f_code->co_filename);
    static const QString DummyFileName = QString::fromLatin1("<program>");
    const bool sameFile = DummyFileName==fileName || fileName==self->sourceProgramPath_;
    bool mustStop = false;

    self->mutex_->lock();
    self->canStepOut_ = frame->f_back != 0;
    self->mutex_->unlock();

    if (!mustStop) {
        if (sameFile) {

            int lineNumber = PyFrame_GetLineNumber(frame) - 1;
            self->mutex_->lock();
            self->lineNumber_ = lineNumber;
            self->mutex_->unlock();

            if (PyTrace_LINE==what || PyTrace_CALL==what || PyTrace_C_CALL==what) {
                // Program not broken, so clear possible previous error flag
                self->mutex_->lock();
                self->errorText_.clear();
                self->mutex_->unlock();
            }

            if (PyTrace_LINE==what)                
                // Notify GUI on line change
                self->dispatchLineChange();

            if (!self->runMode_.isEmpty()) {
                RunMode mode = self->runMode_.last();
                if (RM_StepOver==mode || RM_StepIn==mode) {
                    self->updateDebuggerVariablesModel(frame);
                }
            }

            if (PyTrace_LINE==what && !self->runMode_.isEmpty()) {
                RunMode mode = self->runMode_.last();
                if (RM_StepOver==mode || RM_StepIn==mode) {
                    Q_EMIT self->stopped(RunInterface::SR_UserInteraction);
                    self->runPauseSemaphore_->acquire();
                }
            }

            if (PyTrace_CALL==what) {
                RunMode curMode = RM_StepOver;
                if (!self->runMode_.isEmpty())
                    curMode = self->runMode_.top();
                RunMode nextMode;
                if (RM_Blind == curMode || RM_Regular == curMode)
                    nextMode = curMode;
                else if (RM_StepIn == curMode)
                    nextMode = RM_StepOver;
                else
                    nextMode = RM_StepOut;
                self->runMode_.push(curMode==RM_StepIn ? RM_StepOver : RM_Regular);
            }

            if (PyTrace_RETURN==what && !self->runMode_.isEmpty()) {
                self->runMode_.pop();
            }

            if (PyTrace_EXCEPTION==what || PyTrace_C_EXCEPTION==what) {
                // Set error status
                PyObject * exception = PyTuple_GetItem(arg, 1);
                PyObject * message = PyObject_Str(exception);
                self->mutex_->lock();
                self->errorText_ = PyUnicodeToQString(message);
                self->mutex_->unlock();
                Py_XDECREF(message);
            }
        }
        else {
            // not same file
            self->mutex_->lock();
            if (!self->runMode_.isEmpty() && RM_StepIn==self->runMode_.top()) {
                self->runMode_.top() = RM_StepOver;
            }
            self->mutex_->unlock();
        }
    }

    self->mutex_->lock();
    mustStop = self->stopping_;
    self->mutex_->unlock();

    return mustStop;
}

void PythonRunThread::terminate()
{
    mutex_->lock();
    stopping_ = true;
    mutex_->unlock();

    releaseSemaphores();
}

void PythonRunThread::dispatchLineChange()
{
    mutex_->lock();
    RunMode currentMode = RM_Regular;
    if (!runMode_.isEmpty())
        currentMode = runMode_.top();
    bool emitSteps = RM_StepOver==currentMode || RM_StepIn==currentMode || RM_StepOut==currentMode;
    bool justStarted = false;
    unsigned long int stepsCounter = 0;
    int lineNumber = lineNumber_;
    emitSteps = emitSteps || justStarted_;
    if (!justStarted_) {
        stepsCounted_++;
        stepsCounter = stepsCounted_;
        emitSteps = emitSteps || 0==stepsCounter%1000;
    }
    else {
        justStarted = true;
    }
    justStarted_ = false;
    mutex_->unlock();
    if (justStarted) {
        Q_EMIT updateStepsCounter(0);
    }
    else if (emitSteps) {
        Q_EMIT updateStepsCounter(stepsCounter);
    }

    // Highlight current line if need
    if (RM_StepOver==currentMode || RM_StepIn==currentMode || RM_StepOut==currentMode) {
        Q_EMIT lineChanged(lineNumber, 0, 0);
    }

}

void PythonRunThread::handlePythonInput()
{
    // Request raw input from GUI
    Q_EMIT inputRequest("s");

    // Wait until data written from GUI thread and semaphore reseased
    runInputSemaphore_->acquire();
}

void PythonRunThread::setInputResult(const QVariantList &results)
{
    Q_ASSERT(results.size()==0 || results.size()==1);
    const QString inputString = results.isEmpty() ? QString() : results.at(0).toString();

    callback_->setInputString(inputString);
    runInputSemaphore_->release();
}

void PythonRunThread::updateDebuggerVariablesModel(PyFrameObject *current_frame)
{
    variablesModel_->update(current_frame);
}

void PythonRunThread::startOrContinue(const RunMode runMode)
{    
    if (!isRunning()) {
        runMode_.clear();
        runMode_.push(RM_StepOver==runMode? RM_StepIn : runMode);
        actorsHandler_->resetActors();
        start();
    }
    else {
        if (runMode_.isEmpty())
            runMode_.push(runMode);
        else
            runMode_.top() = runMode;
        runPauseSemaphore_->release();
    }
}

QAbstractItemModel* PythonRunThread::variablesModel() const
{
    return variablesModel_;
}

} // namespace Python3Language