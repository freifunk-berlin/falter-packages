# -*- python -*-
# ex: set filetype=python:

from buildbot.plugins import util, steps
from buildbot.process import build, buildstep, factory, logobserver
from twisted.internet import defer
from twisted.python import log

# Expects the command to print a newline-delimited list of archs / package architectures.
class AsyncBuildGenerator(buildstep.ShellMixin, steps.BuildStep):
    def __init__(self, stepFunc, **kwargs):
        kwargs = self.setupShellMixin(kwargs)
        super().__init__(**kwargs)
        self.observer = logobserver.BufferLogObserver()
        self.addLogObserver('stdio', self.observer)
        self.stepFunc = stepFunc

    def getLines(self, stdout):
        archs = []
        for line in stdout.split('\n'):
            arch = str(line.strip())
            if arch and not arch.startswith('#'):
                archs.append(arch)
        return archs

    @defer.inlineCallbacks
    def run(self):
        cmd = yield self.makeRemoteShellCommand()
        yield self.runCommand(cmd)
        result = cmd.results()
        if result == util.SUCCESS:
            self.build.addStepsAfterCurrentStep([
                self.stepFunc(a) for a in self.getLines(self.observer.getStdout())
            ])
        return result

class AsyncTrigger(steps.Trigger):
    def setAsyncLock(self, lock):
        self.asyncLock = lock

    @defer.inlineCallbacks
    def _createStep(self):
        """
        We need to generate step number with locks as this code
        may now run in parallel.
        """
        self.name = yield self.build.render(self.name)
        self.build.setUniqueStepName(self)
        self.stepid, self.number, self.name = yield self.master.data.updates.addStep(
            buildid=self.build.buildid,
            name=self.name)
            # name=bytes2unicode(self.name))

    @defer.inlineCallbacks
    def addStep(self):
        """
        Create and start the step, noting that the name may be altered to
        ensure uniqueness.
        """
        log.msg("->> calling self.asyncLock.run() <<-")
        yield self.asyncLock.run(self._createStep)
        yield self.master.data.updates.startStep(self.stepid)

class AsyncBuild(build.Build):
    def setupBuild(self):
        """
        Remember async locks and create an async lock itself,
        providing it to the async triggers.
        """
        log.msg("->> inside AsyncBuild.setupBuild() <<-")
        super().setupBuild()

        self.asyncSteps = []
        self.asyncLock = defer.DeferredLock()

        for step in self.steps:
            log.msg(f"->> inside AsyncBuild.setupBuild() - step: {step} <<-")
            if isinstance(step, AsyncTrigger):
                self.asyncSteps.append(step)
                log.msg("->> calling step.setAsyncLock() <<-")
                step.setAsyncLock(self.asyncLock)

    def addStepsAfterCurrentStep(self, steps):
        super().addStepsAfterCurrentStep(steps)
        for step in self.steps:
            if isinstance(step, AsyncTrigger):
                self.asyncSteps.append(step)
                log.msg("->> inside AsyncBuild.addStepsAfterCurrentStep() - calling step.setAsyncLock() <<-")
                step.setAsyncLock(self.asyncLock)

    def stopBuild(self, reason="<no reason given>", results=util.CANCELLED):
        """
        Interrupt not just the current step but also all async steps.
        """
        log.msg(f" {self}: stopping my build: {reason} {results}")
        if self.finished:
            return

        self.stopped = True

        for step in self.asyncSteps:
            step.interrupt(reason)

        return super().stopBuild(reason, results)

    def startNextStep(self):
        """
        Start more than one step when there are async steps in the build.
        """
        while True:
            try:
                s = self.getNextStep()
            except StopIteration:
                s = None
            if s:
                self.executedSteps.append(s)
                self.currentStep = s

                # Run all async steps we have right now.
                if isinstance(s, AsyncTrigger):
                    self._start_next_step_impl(s)
                    continue

                self._start_next_step_impl(s)
                return defer.succeed(None)

            if self.asyncSteps:
                return defer.succeed(None)

            return self.allStepsDone()

    def stepDone(self, results, step):
        """
        Remove step from asyncSteps to ensure last async step
        triggers allStepsDone().
        """
        if isinstance(step, AsyncTrigger):
            self.asyncSteps.remove(step)

        return super().stepDone(results, step)

class AsyncBuildFactory(factory.BuildFactory):
    def __init__(self, steps=None):
        """
        Use custom build factory to permit wrapping build methods.
        """
        log.msg("->> inside AsyncBuildFactory.__init__ <<-")
        super().__init__(steps)
        self.buildClass = AsyncBuild
