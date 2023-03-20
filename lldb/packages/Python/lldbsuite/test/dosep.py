"""
Run the test suite using a separate process for each test file.

Each test will run with a time limit of 10 minutes by default.

Override the default time limit of 10 minutes by setting
the environment variable LLDB_TEST_TIMEOUT.

E.g., export LLDB_TEST_TIMEOUT=10m

Override the time limit for individual tests by setting
the environment variable LLDB_[TEST NAME]_TIMEOUT.

E.g., export LLDB_TESTCONCURRENTEVENTS_TIMEOUT=2m

Set to "0" to run without time limit.

E.g., export LLDB_TEST_TIMEOUT=0
or    export LLDB_TESTCONCURRENTEVENTS_TIMEOUT=0

To collect core files for timed out tests,
do the following before running dosep.py

OSX
ulimit -c unlimited
sudo sysctl -w kern.corefile=core.%P

Linux:
ulimit -c unlimited
echo core.%p | sudo tee /proc/sys/kernel/core_pattern
"""

from __future__ import absolute_import
from __future__ import print_function

# system packages and modules
import asyncore
import distutils.version
import fnmatch
import multiprocessing
import multiprocessing.pool
import os
import platform
import re
import signal
import sys
import threading

from six import StringIO
from six.moves import queue

# Our packages and modules
import lldbsuite
import lldbsuite.support.seven as seven

from . import configuration
from . import dotest_args
from lldbsuite.support import optional_with
from lldbsuite.test_event import dotest_channels
from lldbsuite.test_event.event_builder import EventBuilder
from lldbsuite.test_event import formatter

from .test_runner import process_control

# Status codes for running command with timeout.
eTimedOut, ePassed, eFailed = 124, 0, 1

g_session_dir = None
g_runner_context = None
output_lock = None
test_counter = None
total_tests = None
test_name_len = None
dotest_options = None
RESULTS_FORMATTER = None
RUNNER_PROCESS_ASYNC_MAP = None
RESULTS_LISTENER_CHANNEL = None

"""Contains an optional function pointer that can return the worker index
   for the given thread/process calling it.  Returns a 0-based index."""
GET_WORKER_INDEX = None


def setup_global_variables(
        lock, counter, total, name_len, options, worker_index_map):
    global output_lock, test_counter, total_tests, test_name_len
    global dotest_options
    output_lock = lock
    test_counter = counter
    total_tests = total
    test_name_len = name_len
    dotest_options = options

    if worker_index_map is not None:
        # We'll use the output lock for this to avoid sharing another lock.
        # This won't be used much.
        index_lock = lock

        def get_worker_index_use_pid():
            """Returns a 0-based, process-unique index for the worker."""
            pid = os.getpid()
            with index_lock:
                if pid not in worker_index_map:
                    worker_index_map[pid] = len(worker_index_map)
                return worker_index_map[pid]

        global GET_WORKER_INDEX
        GET_WORKER_INDEX = get_worker_index_use_pid


def report_test_failure(name, command, output, timeout):
    global output_lock
    with output_lock:
        if not (RESULTS_FORMATTER and RESULTS_FORMATTER.is_using_terminal()):
            print(file=sys.stderr)
            print(output, file=sys.stderr)
            if timeout:
                timeout_str = " (TIMEOUT)"
            else:
                timeout_str = ""
            print("[%s FAILED]%s" % (name, timeout_str), file=sys.stderr)
            print("Command invoked: %s" % ' '.join(command), file=sys.stderr)
        update_progress(name)


def report_test_pass(name, output):
    global output_lock
    with output_lock:
        update_progress(name)


def update_progress(test_name=""):
    global output_lock, test_counter, total_tests, test_name_len
    with output_lock:
        counter_len = len(str(total_tests))
        if not (RESULTS_FORMATTER and RESULTS_FORMATTER.is_using_terminal()):
            sys.stderr.write(
                "\r%*d out of %d test suites processed - %-*s" %
                (counter_len, test_counter.value, total_tests,
                 test_name_len.value, test_name))
        if len(test_name) > test_name_len.value:
            test_name_len.value = len(test_name)
        test_counter.value += 1
        sys.stdout.flush()
        sys.stderr.flush()


def parse_test_results(output):
    passes = 0
    failures = 0
    unexpected_successes = 0
    for result in output:
        pass_count = re.search("^RESULT:.*([0-9]+) passes",
                               result, re.MULTILINE)
        fail_count = re.search("^RESULT:.*([0-9]+) failures",
                               result, re.MULTILINE)
        error_count = re.search("^RESULT:.*([0-9]+) errors",
                                result, re.MULTILINE)
        unexpected_success_count = re.search(
            "^RESULT:.*([0-9]+) unexpected successes", result, re.MULTILINE)
        if pass_count is not None:
            passes = passes + int(pass_count.group(1))
        if fail_count is not None:
            failures = failures + int(fail_count.group(1))
        if unexpected_success_count is not None:
            unexpected_successes = unexpected_successes + \
                int(unexpected_success_count.group(1))
        if error_count is not None:
            failures = failures + int(error_count.group(1))
    return passes, failures, unexpected_successes


class DoTestProcessDriver(process_control.ProcessDriver):
    """Drives the dotest.py inferior process and handles bookkeeping."""

    def __init__(self, output_file, output_file_lock, pid_events, file_name,
                 soft_terminate_timeout):
        super(DoTestProcessDriver, self).__init__(
            soft_terminate_timeout=soft_terminate_timeout)
        self.output_file = output_file
        self.output_lock = optional_with.optional_with(output_file_lock)
        self.pid_events = pid_events
        self.results = None
        self.file_name = file_name

    def write(self, content):
        with self.output_lock:
            self.output_file.write(content)

    def on_process_started(self):
        if self.pid_events:
            self.pid_events.put_nowait(('created', self.process.pid))

    def on_process_exited(self, command, output, was_timeout, exit_status):
        if self.pid_events:
            # No point in culling out those with no exit_status (i.e.
            # those we failed to kill). That would just cause
            # downstream code to try to kill it later on a Ctrl-C. At
            # this point, a best-effort-to-kill already took place. So
            # call it destroyed here.
            self.pid_events.put_nowait(('destroyed', self.process.pid))

        # Override the exit status if it was a timeout.
        if was_timeout:
            exit_status = eTimedOut

        # If we didn't end up with any output, call it empty for
        # stdout/stderr.
        if output is None:
            output = ('', '')

        # Now parse the output.
        passes, failures, unexpected_successes = parse_test_results(output)
        if exit_status == 0:
            # stdout does not have any useful information from 'dotest.py',
            # only stderr does.
            report_test_pass(self.file_name, output[1])
        else:
            report_test_failure(
                self.file_name,
                command,
                output[1],
                was_timeout)

        # Save off the results for the caller.
        self.results = (
            self.file_name,
            exit_status,
            passes,
            failures,
            unexpected_successes)

    def on_timeout_pre_kill(self):
        # We're just about to have a timeout take effect.  Here's our chance
        # to do a pre-kill action.

        # For now, we look to see if the lldbsuite.pre_kill module has a
        # runner for our platform.
        module_name = "lldbsuite.pre_kill_hook." + platform.system().lower()
        import importlib
        try:
            module = importlib.import_module(module_name)
        except ImportError:
            # We don't have one for this platform.  Skip.
            sys.stderr.write("\nwarning: no timeout handler module: " +
                             module_name + "\n")
            return

        # Try to run the pre-kill-hook method.
        try:
            # Run the pre-kill command.
            output_io = StringIO()
            module.do_pre_kill(self.pid, g_runner_context, output_io)

            # Write the output to a filename associated with the test file and
            # pid.
            MAX_UNCOMPRESSED_BYTE_COUNT = 10 * 1024

            content = output_io.getvalue()
            compress_output = len(content) > MAX_UNCOMPRESSED_BYTE_COUNT
            basename = "{}-{}.sample".format(self.file_name, self.pid)
            sample_path = os.path.join(g_session_dir, basename)

            if compress_output:
                # Write compressed output into a .zip file.
                from zipfile import ZipFile, ZIP_DEFLATED
                zipfile = sample_path + ".zip"
                with ZipFile(zipfile, "w", ZIP_DEFLATED) as sample_zip:
                    sample_zip.writestr(basename, content)
            else:
                # Write raw output into a text file.
                with open(sample_path, "w") as output_file:
                    output_file.write(content)
        except Exception as e:
            sys.stderr.write("caught exception while running "
                             "pre-kill action: {}\n".format(e))
            return

    def is_exceptional_exit(self):
        """Returns whether the process returned a timeout.

        Not valid to call until after on_process_exited() completes.

        @return True if the exit is an exceptional exit (e.g. signal on
        POSIX); False otherwise.
        """
        if self.results is None:
            raise Exception(
                "exit status checked before results are available")
        return self.process_helper.is_exceptional_exit(
            self.results[1])

    def exceptional_exit_details(self):
        if self.results is None:
            raise Exception(
                "exit status checked before results are available")
        return self.process_helper.exceptional_exit_details(self.results[1])

    def is_timeout(self):
        if self.results is None:
            raise Exception(
                "exit status checked before results are available")
        return self.results[1] == eTimedOut


def get_soft_terminate_timeout():
    # Defaults to 10 seconds, but can set
    # LLDB_TEST_SOFT_TERMINATE_TIMEOUT to a floating point
    # number in seconds.  This value indicates how long
    # the test runner will wait for the dotest inferior to
    # handle a timeout via a soft terminate before it will
    # assume that failed and do a hard terminate.

    # TODO plumb through command-line option
    return float(os.environ.get('LLDB_TEST_SOFT_TERMINATE_TIMEOUT', 10.0))


def want_core_on_soft_terminate():
    # TODO plumb through command-line option
    if platform.system() == 'Linux':
        return True
    else:
        return False


def send_events_to_collector(events, command):
    """Sends the given events to the collector described in the command line.

    @param events the list of events to send to the test event collector.
    @param command the inferior command line which contains the details on
    how to connect to the test event collector.
    """
    if events is None or len(events) == 0:
        # Nothing to do.
        return

    # Find the port we need to connect to from the --results-port option.
    try:
        arg_index = command.index("--results-port") + 1
    except ValueError:
        # There is no results port, so no way to communicate back to
        # the event collector.  This is not a problem if we're not
        # using event aggregation.
        # TODO flag as error once we always use the event system
        print(
            "INFO: no event collector, skipping post-inferior test "
            "event reporting")
        return

    if arg_index >= len(command):
        raise Exception(
            "expected collector port at index {} in {}".format(
                arg_index, command))
    event_port = int(command[arg_index])

    # Create results formatter connected back to collector via socket.
    config = formatter.FormatterConfig()
    config.port = event_port
    formatter_spec = formatter.create_results_formatter(config)
    if formatter_spec is None or formatter_spec.formatter is None:
        raise Exception(
            "Failed to create socket-based ResultsFormatter "
            "back to test event collector")

    # Send the events: the port-based event just pickles the content
    # and sends over to the server side of the socket.
    for event in events:
        formatter_spec.formatter.handle_event(event)

    # Cleanup
    if formatter_spec.cleanup_func is not None:
        formatter_spec.cleanup_func()


def send_inferior_post_run_events(
        command, worker_index, process_driver, test_filename):
    """Sends any test events that should be generated after the inferior runs.

    These events would include timeouts and exceptional (i.e. signal-returning)
    process completion results.

    @param command the list of command parameters passed to subprocess.Popen().
    @param worker_index the worker index (possibly None) used to run
    this process
    @param process_driver the ProcessDriver-derived instance that was used
    to run the inferior process.
    @param test_filename the full path to the Python test file that is being
    run.
    """
    if process_driver is None:
        raise Exception("process_driver must not be None")
    if process_driver.results is None:
        # Invalid condition - the results should have been set one way or
        # another, even in a timeout.
        raise Exception("process_driver.results were not set")

    # The code below fills in the post events struct.  If there are any post
    # events to fire up, we'll try to make a connection to the socket and
    # provide the results.
    post_events = []

    # Handle signal/exceptional exits.
    if process_driver.is_exceptional_exit():
        (code, desc) = process_driver.exceptional_exit_details()
        post_events.append(
            EventBuilder.event_for_job_exceptional_exit(
                process_driver.pid,
                worker_index,
                code,
                desc,
                test_filename,
                command))

    # Handle timeouts.
    if process_driver.is_timeout():
        post_events.append(EventBuilder.event_for_job_timeout(
            process_driver.pid,
            worker_index,
            test_filename,
            command))

    if len(post_events) > 0:
        send_events_to_collector(post_events, command)


def call_with_timeout(
        command, timeout, name, inferior_pid_events, test_filename):
    # Add our worker index (if we have one) to all test events
    # from this inferior.
    worker_index = None
    if GET_WORKER_INDEX is not None:
        try:
            worker_index = GET_WORKER_INDEX()
            command.extend([
                "--event-add-entries",
                "worker_index={}:int".format(worker_index)])
        except:  # pylint: disable=bare-except
            # Ctrl-C does bad things to multiprocessing.Manager.dict()
            # lookup.  Just swallow it.
            pass

    # Create the inferior dotest.py ProcessDriver.
    soft_terminate_timeout = get_soft_terminate_timeout()
    want_core = want_core_on_soft_terminate()

    process_driver = DoTestProcessDriver(
        sys.stdout,
        output_lock,
        inferior_pid_events,
        name,
        soft_terminate_timeout)

    # Run it with a timeout.
    process_driver.run_command_with_timeout(command, timeout, want_core)

    # Return the results.
    if not process_driver.results:
        # This is truly exceptional.  Even a failing or timed out
        # binary should have called the results-generation code.
        raise Exception("no test results were generated whatsoever")

    # Handle cases where the test inferior cannot adequately provide
    # meaningful results to the test event system.
    send_inferior_post_run_events(
        command,
        worker_index,
        process_driver,
        test_filename)

    return process_driver.results


def process_file(test_file, dotest_argv, inferior_pid_events):
    """Run tests in the specified file in a subprocess and gather the results."""
    results = []
    base_name = os.path.basename(test_file)

    import __main__ as main
    global dotest_options
    if not dotest_options.p or re.search(dotest_options.p, base_name):
        script_file = main.__file__
        command = ([sys.executable, script_file] +
                   dotest_argv +
                   ["-S", dotest_options.session_file_format] +
                   ["--inferior", "-p", base_name, os.path.dirname(test_file)])

        timeout_name = os.path.basename(os.path.splitext(base_name)[0]).upper()

        timeout = (os.getenv("LLDB_%s_TIMEOUT" % timeout_name) or
                   getDefaultTimeout(dotest_options.lldb_platform_name))

        results.append(call_with_timeout(
            command, timeout, base_name, inferior_pid_events, test_file))

    # result = (name, status, passes, failures, unexpected_successes)
    timed_out = [name for name, status, _, _, _ in results
                 if status == eTimedOut]
    passed = [name for name, status, _, _, _ in results
              if status == ePassed]
    failed = [name for name, status, _, _, _ in results
              if status != ePassed]
    unexpected_passes = [
        name for name, _, _, _, unexpected_successes in results
        if unexpected_successes > 0]

    pass_count = sum([result[2] for result in results])
    fail_count = sum([result[3] for result in results])

    return (
        timed_out, passed, failed, unexpected_passes, pass_count, fail_count)

in_q = None
out_q = None


def process_dir_worker_multiprocessing(
        a_output_lock, a_test_counter, a_total_tests, a_test_name_len,
        a_dotest_options, job_queue, result_queue, inferior_pid_events,
        worker_index_map):
    """Worker thread main loop when in multiprocessing mode.
    Takes one directory specification at a time and works on it."""

    # Shut off interrupt handling in the child process.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    if hasattr(signal, 'SIGHUP'):
        signal.signal(signal.SIGHUP, signal.SIG_IGN)

    # Setup the global state for the worker process.
    setup_global_variables(
        a_output_lock, a_test_counter, a_total_tests, a_test_name_len,
        a_dotest_options, worker_index_map)

    # Keep grabbing entries from the queue until done.
    while not job_queue.empty():
        try:
            job = job_queue.get(block=False)
            result = process_file(job[0], job[1], job[2],
                                 inferior_pid_events)
            result_queue.put(result)
        except queue.Empty:
            # Fine, we're done.
            pass


def process_file_worker_multiprocessing_pool(args):
    return process_file(*args)


def process_file_worker_threading(job_queue, result_queue, inferior_pid_events):
    """Worker thread main loop when in threading mode.

    This one supports the hand-rolled pooling support.

    Takes one directory specification at a time and works on it."""

    # Keep grabbing entries from the queue until done.
    while not job_queue.empty():
        try:
            job = job_queue.get(block=False)
            result = process_file(job[0], job[1], inferior_pid_events)
            result_queue.put(result)
        except queue.Empty:
            # Fine, we're done.
            pass


def process_file_worker_threading_pool(args):
    return process_file(*args)


def process_file_mapper_inprocess(args):
    """Map adapter for running the subprocess-based, non-threaded test runner.

    @param args the process work item tuple
    @return the test result tuple
    """
    return process_file(*args)


def collect_active_pids_from_pid_events(event_queue):
    """
    Returns the set of what should be active inferior pids based on
    the event stream.

    @param event_queue a multiprocessing.Queue containing events of the
    form:
         ('created', pid)
         ('destroyed', pid)

    @return set of inferior dotest.py pids activated but never completed.
    """
    active_pid_set = set()
    while not event_queue.empty():
        pid_event = event_queue.get_nowait()
        if pid_event[0] == 'created':
            active_pid_set.add(pid_event[1])
        elif pid_event[0] == 'destroyed':
            active_pid_set.remove(pid_event[1])
    return active_pid_set


def kill_all_worker_processes(workers, inferior_pid_events):
    """
    Kills all specified worker processes and their process tree.

    @param workers a list of multiprocess.Process worker objects.
    @param inferior_pid_events a multiprocess.Queue that contains
    all inferior create and destroy events.  Used to construct
    the list of child pids still outstanding that need to be killed.
    """
    for worker in workers:
        worker.terminate()
        worker.join()

    # Add all the child test pids created.
    active_pid_set = collect_active_pids_from_pid_events(
        inferior_pid_events)
    for inferior_pid in active_pid_set:
        print("killing inferior pid {}".format(inferior_pid))
        os.kill(inferior_pid, signal.SIGKILL)


def kill_all_worker_threads(workers, inferior_pid_events):
    """
    Kills all specified worker threads and their process tree.

    @param workers a list of multiprocess.Process worker objects.
    @param inferior_pid_events a multiprocess.Queue that contains
    all inferior create and destroy events.  Used to construct
    the list of child pids still outstanding that need to be killed.
    """

    # Add all the child test pids created.
    active_pid_set = collect_active_pids_from_pid_events(
        inferior_pid_events)
    for inferior_pid in active_pid_set:
        print("killing inferior pid {}".format(inferior_pid))
        os.kill(inferior_pid, signal.SIGKILL)

    # We don't have a way to nuke the threads.  However, since we killed
    # all the inferiors, and we drained the job queue, this will be
    # good enough.  Wait cleanly for each worker thread to wrap up.
    for worker in workers:
        worker.join()


def find_test_files_in_dir_tree(dir_root):
    """Returns all the test files in the given dir hierarchy.

    @param dir_root the path to the directory to start scanning
    for test files.  All files in this directory and all its children
    directory trees will be searched.
    """
    for root, _, files in os.walk(dir_root, topdown=False):
        def is_test_filename(test_dir, base_filename):
            """Returns True if the given filename matches the test name format.

            @param test_dir the directory to check.  Should be absolute or
            relative to current working directory.

            @param base_filename the base name of the filename to check for a
            dherence to the python test case filename format.

            @return True if name matches the python test case filename format.
            """
            # Not interested in symbolically linked files.
            if os.path.islink(os.path.join(test_dir, base_filename)):
                return False
            # Only interested in test files with the "Test*.py" naming pattern.
            return (base_filename.startswith("Test") and
                    base_filename.endswith(".py"))

        for f in files:
            if is_test_filename(root, f):
                yield os.path.join(root, f)


def initialize_global_vars_common(num_threads, test_work_items, session_dir,
                                  runner_context):
    global g_session_dir, g_runner_context, total_tests, test_counter
    global test_name_len

    total_tests = len(test_work_items)
    test_counter = multiprocessing.Value('i', 0)
    test_name_len = multiprocessing.Value('i', 0)
    g_session_dir = session_dir
    g_runner_context = runner_context
    if not (RESULTS_FORMATTER and RESULTS_FORMATTER.is_using_terminal()):
        print(
            "Testing: %d test suites, %d thread%s" %
            (total_tests,
             num_threads,
             (num_threads > 1) *
                "s"),
            file=sys.stderr)
    update_progress()


def initialize_global_vars_multiprocessing(num_threads, test_work_items,
                                           session_dir, runner_context):
    # Initialize the global state we'll use to communicate with the
    # rest of the flat module.
    global output_lock
    output_lock = multiprocessing.RLock()

    initialize_global_vars_common(num_threads, test_work_items, session_dir,
                                  runner_context)


def initialize_global_vars_threading(num_threads, test_work_items, session_dir,
                                     runner_context):
    """Initializes global variables used in threading mode.

    @param num_threads specifies the number of workers used.

    @param test_work_items specifies all the work items
    that will be processed.

    @param session_dir the session directory where test-run-speciif files are
    written.

    @param runner_context a dictionary of platform-related data that is passed
    to the timeout pre-kill hook.
    """
    # Initialize the global state we'll use to communicate with the
    # rest of the flat module.
    global output_lock
    output_lock = threading.RLock()

    index_lock = threading.RLock()
    index_map = {}

    def get_worker_index_threading():
        """Returns a 0-based, thread-unique index for the worker thread."""
        thread_id = threading.current_thread().ident
        with index_lock:
            if thread_id not in index_map:
                index_map[thread_id] = len(index_map)
            return index_map[thread_id]

    global GET_WORKER_INDEX
    GET_WORKER_INDEX = get_worker_index_threading

    initialize_global_vars_common(num_threads, test_work_items, session_dir,
                                  runner_context)


def ctrl_c_loop(main_op_func, done_func, ctrl_c_handler):
    """Provides a main loop that is Ctrl-C protected.

    The main loop calls the main_op_func() repeatedly until done_func()
    returns true.  The ctrl_c_handler() method is called with a single
    int parameter that contains the number of times the ctrl_c has been
    hit (starting with 1).  The ctrl_c_handler() should mutate whatever
    it needs to have the done_func() return True as soon as it is desired
    to exit the loop.
    """
    done = False
    ctrl_c_count = 0

    while not done:
        try:
            # See if we're done.  Start with done check since it is
            # the first thing executed after a Ctrl-C handler in the
            # following loop.
            done = done_func()
            if not done:
                # Run the main op once.
                main_op_func()

        except KeyboardInterrupt:
            ctrl_c_count += 1
            ctrl_c_handler(ctrl_c_count)


def pump_workers_and_asyncore_map(workers, asyncore_map):
    """Prunes out completed workers and maintains the asyncore loop.

    The asyncore loop contains the optional socket listener
    and handlers.  When all workers are complete, this method
    takes care of stopping the listener.  It also runs the
    asyncore loop for the given async map for 10 iterations.

    @param workers the list of worker Thread/Process instances.

    @param asyncore_map the asyncore threading-aware map that
    indicates which channels are in use and still alive.
    """

    # Check on all the workers, removing them from the workers
    # list as they complete.
    dead_workers = []
    for worker in workers:
        # This non-blocking join call is what allows us
        # to still receive keyboard interrupts.
        worker.join(0.01)
        if not worker.is_alive():
            dead_workers.append(worker)
            # Clear out the completed workers
    for dead_worker in dead_workers:
        workers.remove(dead_worker)

    # If there are no more workers and there is a listener,
    # close the listener.
    global RESULTS_LISTENER_CHANNEL
    if len(workers) == 0 and RESULTS_LISTENER_CHANNEL is not None:
        RESULTS_LISTENER_CHANNEL.close()
        RESULTS_LISTENER_CHANNEL = None

    # Pump the asyncore map if it isn't empty.
    if len(asyncore_map) > 0:
        asyncore.loop(0.1, False, asyncore_map, 10)


def handle_ctrl_c(ctrl_c_count, job_queue, workers, inferior_pid_events,
                  stop_all_inferiors_func):
    """Performs the appropriate ctrl-c action for non-pool parallel test runners

    @param ctrl_c_count starting with 1, indicates the number of times ctrl-c
    has been intercepted.  The value is 1 on the first intercept, 2 on the
    second, etc.

    @param job_queue a Queue object that contains the work still outstanding
    (i.e. hasn't been assigned to a worker yet).

    @param workers list of Thread or Process workers.

    @param inferior_pid_events specifies a Queue of inferior process
    construction and destruction events.  Used to build the list of inferior
    processes that should be killed if we get that far.

    @param stop_all_inferiors_func a callable object that takes the
    workers and inferior_pid_events parameters (in that order) if a hard
    stop is to be used on the workers.
    """

    # Print out which Ctrl-C we're handling.
    key_name = [
        "first",
        "second",
        "third",
        "many"]

    if ctrl_c_count < len(key_name):
        name_index = ctrl_c_count - 1
    else:
        name_index = len(key_name) - 1
    message = "\nHandling {} KeyboardInterrupt".format(key_name[name_index])
    with output_lock:
        print(message)

    if ctrl_c_count == 1:
        # Remove all outstanding items from the work queue so we stop
        # doing any more new work.
        while not job_queue.empty():
            try:
                # Just drain it to stop more work from being started.
                job_queue.get_nowait()
            except queue.Empty:
                pass
        with output_lock:
            print("Stopped more work from being started.")
    elif ctrl_c_count == 2:
        # Try to stop all inferiors, even the ones currently doing work.
        stop_all_inferiors_func(workers, inferior_pid_events)
    else:
        with output_lock:
            print("All teardown activities kicked off, should finish soon.")


def workers_and_async_done(workers, async_map):
    """Returns True if the workers list and asyncore channels are all done.

    @param workers list of workers (threads/processes).  These must adhere
    to the threading Thread or multiprocessing.Process interface.

    @param async_map the threading-aware asyncore channel map to check
    for live channels.

    @return False if the workers list exists and has any entries in it, or
    if the async_map exists and has any entries left in it; otherwise, True.
    """
    if workers is not None and len(workers) > 0:
        # We're not done if we still have workers left.
        return False
    if async_map is not None and len(async_map) > 0:
        return False
    # We're done.
    return True


def multiprocessing_test_runner(num_threads, test_work_items, session_dir,
                                runner_context):
    """Provides hand-wrapped pooling test runner adapter with Ctrl-C support.

    This concurrent test runner is based on the multiprocessing
    library, and rolls its own worker pooling strategy so it
    can handle Ctrl-C properly.

    This test runner is known to have an issue running on
    Windows platforms.

    @param num_threads the number of worker processes to use.

    @param test_work_items the iterable of test work item tuples
    to run.

    @param session_dir the session directory where test-run-speciif files are
    written.

    @param runner_context a dictionary of platform-related data that is passed
    to the timeout pre-kill hook.
    """

    # Initialize our global state.
    initialize_global_vars_multiprocessing(num_threads, test_work_items,
                                           session_dir, runner_context)

    # Create jobs.
    job_queue = multiprocessing.Queue(len(test_work_items))
    for test_work_item in test_work_items:
        job_queue.put(test_work_item)

    result_queue = multiprocessing.Queue(len(test_work_items))

    # Create queues for started child pids.  Terminating
    # the multiprocess processes does not terminate the
    # child processes they spawn.  We can remove this tracking
    # if/when we move to having the multiprocess process directly
    # perform the test logic.  The Queue size needs to be able to
    # hold 2 * (num inferior dotest.py processes started) entries.
    inferior_pid_events = multiprocessing.Queue(4096)

    # Worker dictionary allows each worker to figure out its worker index.
    manager = multiprocessing.Manager()
    worker_index_map = manager.dict()

    # Create workers.  We don't use multiprocessing.Pool due to
    # challenges with handling ^C keyboard interrupts.
    workers = []
    for _ in range(num_threads):
        worker = multiprocessing.Process(
            target=process_file_worker_multiprocessing,
            args=(output_lock,
                  test_counter,
                  total_tests,
                  test_name_len,
                  dotest_options,
                  job_queue,
                  result_queue,
                  inferior_pid_events,
                  worker_index_map))
        worker.start()
        workers.append(worker)

    # Main loop: wait for all workers to finish and wait for
    # the socket handlers to wrap up.
    ctrl_c_loop(
        # Main operation of loop
        lambda: pump_workers_and_asyncore_map(
            workers, RUNNER_PROCESS_ASYNC_MAP),

        # Return True when we're done with the main loop.
        lambda: workers_and_async_done(workers, RUNNER_PROCESS_ASYNC_MAP),

        # Indicate what we do when we receive one or more Ctrl-Cs.
        lambda ctrl_c_count: handle_ctrl_c(
            ctrl_c_count, job_queue, workers, inferior_pid_events,
            kill_all_worker_processes))

    # Reap the test results.
    test_results = []
    while not result_queue.empty():
        test_results.append(result_queue.get(block=False))
    return test_results


def map_async_run_loop(future, channel_map, listener_channel):
    """Blocks until the Pool.map_async completes and the channel completes.

    @param future an AsyncResult instance from a Pool.map_async() call.

    @param channel_map the asyncore dispatch channel map that should be pumped.
    Optional: may be None.

    @param listener_channel the channel representing a listener that should be
    closed once the map_async results are available.

    @return the results from the async_result instance.
    """
    map_results = None

    done = False
    while not done:
        # Check if we need to reap the map results.
        if map_results is None:
            if future.ready():
                # Get the results.
                map_results = future.get()

                # Close the runner process listener channel if we have
                # one: no more connections will be incoming.
                if listener_channel is not None:
                    listener_channel.close()

        # Pump the asyncore loop if we have a listener socket.
        if channel_map is not None:
            asyncore.loop(0.01, False, channel_map, 10)

        # Figure out if we're done running.
        done = map_results is not None
        if channel_map is not None:
            # We have a runner process async map.  Check if it
            # is complete.
            if len(channel_map) > 0:
                # We still have an asyncore channel running.  Not done yet.
                done = False

    return map_results


def multiprocessing_test_runner_pool(num_threads, test_work_items, session_dir,
                                     runner_context):
    # Initialize our global state.
    initialize_global_vars_multiprocessing(num_threads, test_work_items,
                                           session_dir, runner_context)

    manager = multiprocessing.Manager()
    worker_index_map = manager.dict()

    pool = multiprocessing.Pool(
        num_threads,
        initializer=setup_global_variables,
        initargs=(output_lock, test_counter, total_tests, test_name_len,
                  dotest_options, worker_index_map))

    # Start the map operation (async mode).
    map_future = pool.map_async(
        process_file_worker_multiprocessing_pool, test_work_items)
    return map_async_run_loop(
        map_future, RUNNER_PROCESS_ASYNC_MAP, RESULTS_LISTENER_CHANNEL)


def threading_test_runner(num_threads, test_work_items, session_dir,
                          runner_context):
    """Provides hand-wrapped pooling threading-based test runner adapter
    with Ctrl-C support.

    This concurrent test runner is based on the threading
    library, and rolls its own worker pooling strategy so it
    can handle Ctrl-C properly.

    @param num_threads the number of worker processes to use.

    @param test_work_items the iterable of test work item tuples
    to run.

    @param session_dir the session directory where test-run-speciif files are
    written.

    @param runner_context a dictionary of platform-related data that is passed
    to the timeout pre-kill hook.
   """

    # Initialize our global state.
    initialize_global_vars_threading(num_threads, test_work_items, session_dir,
                                     runner_context)

    # Create jobs.
    job_queue = queue.Queue()
    for test_work_item in test_work_items:
        job_queue.put(test_work_item)

    result_queue = queue.Queue()

    # Create queues for started child pids.  Terminating
    # the threading threads does not terminate the
    # child processes they spawn.
    inferior_pid_events = queue.Queue()

    # Create workers. We don't use multiprocessing.pool.ThreadedPool
    # due to challenges with handling ^C keyboard interrupts.
    workers = []
    for _ in range(num_threads):
        worker = threading.Thread(
            target=process_file_worker_threading,
            args=(job_queue,
                  result_queue,
                  inferior_pid_events))
        worker.start()
        workers.append(worker)

    # Main loop: wait for all workers to finish and wait for
    # the socket handlers to wrap up.
    ctrl_c_loop(
        # Main operation of loop
        lambda: pump_workers_and_asyncore_map(
            workers, RUNNER_PROCESS_ASYNC_MAP),

        # Return True when we're done with the main loop.
        lambda: workers_and_async_done(workers, RUNNER_PROCESS_ASYNC_MAP),

        # Indicate what we do when we receive one or more Ctrl-Cs.
        lambda ctrl_c_count: handle_ctrl_c(
            ctrl_c_count, job_queue, workers, inferior_pid_events,
            kill_all_worker_threads))

    # Reap the test results.
    test_results = []
    while not result_queue.empty():
        test_results.append(result_queue.get(block=False))
    return test_results


def threading_test_runner_pool(num_threads, test_work_items, session_dir,
                               runner_context):
    # Initialize our global state.
    initialize_global_vars_threading(num_threads, test_work_items, session_dir,
                                     runner_context)

    pool = multiprocessing.pool.ThreadPool(num_threads)
    map_future = pool.map_async(
        process_file_worker_threading_pool, test_work_items)

    return map_async_run_loop(
        map_future, RUNNER_PROCESS_ASYNC_MAP, RESULTS_LISTENER_CHANNEL)


def asyncore_run_loop(channel_map):
    try:
        asyncore.loop(None, False, channel_map)
    except:
        # Swallow it, we're seeing:
        #   error: (9, 'Bad file descriptor')
        # when the listener channel is closed.  Shouldn't be the case.
        pass


def inprocess_exec_test_runner(test_work_items, session_dir, runner_context):
    # Initialize our global state.
    initialize_global_vars_multiprocessing(1, test_work_items, session_dir,
                                           runner_context)

    # We're always worker index 0
    def get_single_worker_index():
        return 0

    global GET_WORKER_INDEX
    GET_WORKER_INDEX = get_single_worker_index

    # Run the listener and related channel maps in a separate thread.
    # global RUNNER_PROCESS_ASYNC_MAP
    global RESULTS_LISTENER_CHANNEL
    if RESULTS_LISTENER_CHANNEL is not None:
        socket_thread = threading.Thread(
            target=lambda: asyncore_run_loop(RUNNER_PROCESS_ASYNC_MAP))
        socket_thread.start()

    # Do the work.
    test_results = list(map(process_file_mapper_inprocess, test_work_items))

    # If we have a listener channel, shut it down here.
    if RESULTS_LISTENER_CHANNEL is not None:
        # Close down the channel.
        RESULTS_LISTENER_CHANNEL.close()
        RESULTS_LISTENER_CHANNEL = None

        # Wait for the listener and handlers to complete.
        socket_thread.join()

    return test_results


def walk_and_invoke(test_files, dotest_argv, num_workers, test_runner_func):
    """Invokes the test runner on each test file specified by test_files.

    @param test_files a list of (test_file, full_path_to_test_file)
    @param num_workers the number of worker queues working on these test files
    @param test_runner_func the test runner configured to run the tests

    @return a tuple of results from the running of the specified tests,
    of the form (timed_out, passed, failed, unexpected_successes, pass_count,
    fail_count)
    """
    # The async_map is important to keep all thread-related asyncore
    # channels distinct when we call asyncore.loop() later on.
    global RESULTS_LISTENER_CHANNEL, RUNNER_PROCESS_ASYNC_MAP
    RUNNER_PROCESS_ASYNC_MAP = {}

    # If we're outputting side-channel test results, create the socket
    # listener channel and tell the inferior to send results to the
    # port on which we'll be listening.
    if RESULTS_FORMATTER is not None:
        forwarding_func = RESULTS_FORMATTER.handle_event
        RESULTS_LISTENER_CHANNEL = (
            dotest_channels.UnpicklingForwardingListenerChannel(
                RUNNER_PROCESS_ASYNC_MAP, "localhost", 0,
                2 * num_workers, forwarding_func))
        # Set the results port command line arg.  Might have been
        # inserted previous, so first try to replace.
        listener_port = str(RESULTS_LISTENER_CHANNEL.address[1])
        try:
            port_value_index = dotest_argv.index("--results-port") + 1
            dotest_argv[port_value_index] = listener_port
        except ValueError:
            # --results-port doesn't exist (yet), add it
            dotest_argv.append("--results-port")
            dotest_argv.append(listener_port)

    # Build the test work items out of the (dir, file_list) entries passed in.
    test_work_items = []
    for test_file in test_files:
        test_work_items.append((test_file, dotest_argv, None))

    # Convert test work items into test results using whatever
    # was provided as the test run function.
    test_results = test_runner_func(test_work_items)

    # Summarize the results and return to caller.
    timed_out = sum([result[0] for result in test_results], [])
    passed = sum([result[1] for result in test_results], [])
    failed = sum([result[2] for result in test_results], [])
    unexpected_successes = sum([result[3] for result in test_results], [])
    pass_count = sum([result[4] for result in test_results])
    fail_count = sum([result[5] for result in test_results])

    return (timed_out, passed, failed, unexpected_successes, pass_count,
            fail_count)


def getExpectedTimeouts(platform_name):
    # returns a set of test filenames that might timeout
    # are we running against a remote target?

    # Figure out the target system for which we're collecting
    # the set of expected timeout test filenames.
    if platform_name is None:
        target = sys.platform
    else:
        m = re.search(r'remote-(\w+)', platform_name)
        if m is not None:
            target = m.group(1)
        else:
            target = platform_name

    expected_timeout = set()

    if target.startswith("freebsd"):
        expected_timeout |= {
            "TestBreakpointConditions.py",
            "TestChangeProcessGroup.py",
            "TestValueObjectRecursion.py",
            "TestWatchpointConditionAPI.py",
        }
    return expected_timeout


def getDefaultTimeout(platform_name):
    if os.getenv("LLDB_TEST_TIMEOUT"):
        return os.getenv("LLDB_TEST_TIMEOUT")

    if platform_name is None:
        platform_name = sys.platform

    if platform_name.startswith("remote-"):
        return "10m"
    elif platform_name == 'darwin':
        # We are consistently needing more time on a few tests.
        return "6m"
    else:
        return "4m"


def touch(fname, times=None):
    if os.path.exists(fname):
        os.utime(fname, times)


def find(pattern, path):
    result = []
    for root, dirs, files in os.walk(path):
        for name in files:
            if fnmatch.fnmatch(name, pattern):
                result.append(os.path.join(root, name))
    return result


def get_test_runner_strategies(num_threads, session_dir, runner_context):
    """Returns the test runner strategies by name in a dictionary.

    @param num_threads specifies the number of threads/processes
    that will be used for concurrent test runners.

    @param session_dir specifies the session dir to use for
    auxiliary files.

    @param runner_context a dictionary of details on the architectures and
    platform used to run the test suite.  This is passed along verbatim to
    the timeout pre-kill handler, allowing that decoupled component to do
    process inspection in a platform-specific way.

    @return dictionary with key as test runner strategy name and
    value set to a callable object that takes the test work item
    and returns a test result tuple.
    """
    return {
        # multiprocessing supports ctrl-c and does not use
        # multiprocessing.Pool.
        "multiprocessing":
        (lambda work_items: multiprocessing_test_runner(
            num_threads, work_items, session_dir, runner_context)),

        # multiprocessing-pool uses multiprocessing.Pool but
        # does not support Ctrl-C.
        "multiprocessing-pool":
        (lambda work_items: multiprocessing_test_runner_pool(
            num_threads, work_items, session_dir, runner_context)),

        # threading uses a hand-rolled worker pool much
        # like multiprocessing, but instead uses in-process
        # worker threads.  This one supports Ctrl-C.
        "threading":
        (lambda work_items: threading_test_runner(
            num_threads, work_items, session_dir, runner_context)),

        # threading-pool uses threading for the workers (in-process)
        # and uses the multiprocessing.pool thread-enabled pool.
        # This does not properly support Ctrl-C.
        "threading-pool":
        (lambda work_items: threading_test_runner_pool(
            num_threads, work_items, session_dir, runner_context)),

        # serial uses the subprocess-based, single process
        # test runner.  This provides process isolation but
        # no concurrent test execution.
        "serial":
        (lambda work_items: inprocess_exec_test_runner(
            work_items, session_dir, runner_context))
    }


def _remove_option(
        args, long_option_name, short_option_name, takes_arg):
    """Removes option and related option arguments from args array.

    This method removes all short/long options that match the given
    arguments.

    @param args the array of command line arguments (in/out)

    @param long_option_name the full command line representation of the
    long-form option that will be removed (including '--').

    @param short_option_name the short version of the command line option
    that will be removed (including '-').

    @param takes_arg True if the option takes an argument.

    """
    if long_option_name is not None:
        regex_string = "^" + long_option_name + "="
        long_regex = re.compile(regex_string)
    if short_option_name is not None:
        # Short options we only match the -X and assume
        # any arg is one command line argument jammed together.
        # i.e. -O--abc=1 is a single argument in the args list.
        # We don't handle -O --abc=1, as argparse doesn't handle
        # it, either.
        regex_string = "^" + short_option_name
        short_regex = re.compile(regex_string)

    def remove_long_internal():
        """Removes one matching long option from args.
        @returns True if one was found and removed; False otherwise.
        """
        try:
            index = args.index(long_option_name)
            # Handle the exact match case.
            if takes_arg:
                removal_count = 2
            else:
                removal_count = 1
            del args[index:index + removal_count]
            return True
        except ValueError:
            # Thanks to argparse not handling options with known arguments
            # like other options parsing libraries (see
            # https://bugs.python.org/issue9334), we need to support the
            # --results-formatter-options={second-level-arguments} (note
            # the equal sign to fool the first-level arguments parser into
            # not treating the second-level arguments as first-level
            # options). We're certainly at risk of getting this wrong
            # since now we're forced into the business of trying to figure
            # out what is an argument (although I think this
            # implementation will suffice).
            for index in range(len(args)):
                match = long_regex.search(args[index])
                if match:
                    del args[index]
                    return True
            return False

    def remove_short_internal():
        """Removes one matching short option from args.
        @returns True if one was found and removed; False otherwise.
        """
        for index in range(len(args)):
            match = short_regex.search(args[index])
            if match:
                del args[index]
                return True
        return False

    removal_count = 0
    while long_option_name is not None and remove_long_internal():
        removal_count += 1
    while short_option_name is not None and remove_short_internal():
        removal_count += 1
    if removal_count == 0:
        raise Exception(
            "failed to find at least one of '{}', '{}' in options".format(
                long_option_name, short_option_name))


def adjust_inferior_options(dotest_argv):
    """Adjusts the commandline args array for inferiors.

    This method adjusts the inferior dotest commandline options based
    on the parallel test runner's options.  Some of the inferior options
    will need to change to properly handle aggregation functionality.
    """
    global dotest_options

    # If we don't have a session directory, create one.
    if not dotest_options.s:
        # no session log directory, we need to add this to prevent
        # every dotest invocation from creating its own directory
        import datetime
        # The windows platforms don't like ':' in the pathname.
        timestamp_started = (datetime.datetime.now()
                             .strftime("%Y-%m-%d-%H_%M_%S"))
        dotest_argv.append('-s')
        dotest_argv.append(timestamp_started)
        dotest_options.s = timestamp_started

    # Adjust inferior results formatter options - if the parallel
    # test runner is collecting into the user-specified test results,
    # we'll have inferiors spawn with the --results-port option and
    # strip the original test runner options.
    if dotest_options.results_file is not None:
        _remove_option(dotest_argv, "--results-file", None, True)
    if dotest_options.results_port is not None:
        _remove_option(dotest_argv, "--results-port", None, True)
    if dotest_options.results_formatter is not None:
        _remove_option(dotest_argv, "--results-formatter", None, True)
    if dotest_options.results_formatter_options is not None:
        _remove_option(dotest_argv, "--results-formatter-option", "-O",
                       True)

    # Remove the --curses shortcut if specified.
    if dotest_options.curses:
        _remove_option(dotest_argv, "--curses", None, False)

    # Remove test runner name if present.
    if dotest_options.test_runner_name is not None:
        _remove_option(dotest_argv, "--test-runner-name", None, True)


def is_darwin_version_lower_than(target_version):
    """Checks that os is Darwin and version is lower than target_version.

    @param target_version the StrictVersion indicating the version
    we're checking against.

    @return True if the OS is Darwin (OS X) and the version number of
    the OS is less than target_version; False in all other cases.
    """
    if platform.system() != 'Darwin':
        # Can't be Darwin lower than a certain version.
        return False

    system_version = distutils.version.StrictVersion(platform.mac_ver()[0])
    return seven.cmp_(system_version, target_version) < 0


def default_test_runner_name(num_threads):
    """Returns the default test runner name for the configuration.

    @param num_threads the number of threads/workers this test runner is
    supposed to use.

    @return the test runner name that should be used by default when
    no test runner was explicitly called out on the command line.
    """
    if num_threads == 1:
        # Use the serial runner.
        test_runner_name = "serial"
    elif os.name == "nt":
        # On Windows, Python uses CRT with a low limit on the number of open
        # files.  If you have a lot of cores, the threading-pool runner will
        # often fail because it exceeds that limit.  It's not clear what the
        # right balance is, so until we can investigate it more deeply,
        # just use the one that works
        test_runner_name = "multiprocessing-pool"
    elif is_darwin_version_lower_than(
            distutils.version.StrictVersion("10.10.0")):
        # OS X versions before 10.10 appear to have an issue using
        # the threading test runner.  Fall back to multiprocessing.
        # Supports Ctrl-C.
        test_runner_name = "multiprocessing"
    else:
        # For everyone else, use the ctrl-c-enabled threading support.
        # Should use fewer system resources than the multprocessing
        # variant.
        test_runner_name = "threading"
    return test_runner_name


def rerun_tests(test_subdir, tests_for_rerun, dotest_argv, session_dir,
                runner_context):
    # Build the list of test files to rerun.  Some future time we'll
    # enable re-run by test method so we can constrain the rerun set
    # to just the method(s) that were in issued within a file.

    # Sort rerun files into subdirectories.
    print("\nRerunning the following files:")
    rerun_files = []
    for test_filename in tests_for_rerun.keys():
        # Print the file we'll be rerunning
        test_relative_path = os.path.relpath(
            test_filename, lldbsuite.lldb_test_root)
        print("  {}".format(test_relative_path))

        rerun_files.append(test_filename)

    # Do not update legacy counts, I am getting rid of
    # them so no point adding complicated merge logic here.
    rerun_thread_count = 1
    # Force the parallel test runner to choose a multi-worker strategy.
    rerun_runner_name = default_test_runner_name(rerun_thread_count + 1)
    print("rerun will use the '{}' test runner strategy".format(
        rerun_runner_name))

    runner_strategies_by_name = get_test_runner_strategies(
        rerun_thread_count, session_dir, runner_context)
    rerun_runner_func = runner_strategies_by_name[
        rerun_runner_name]
    if rerun_runner_func is None:
        raise Exception(
            "failed to find rerun test runner "
            "function named '{}'".format(rerun_runner_name))

    walk_and_invoke(
        rerun_files,
        dotest_argv,
        rerun_thread_count,
        rerun_runner_func)
    print("\nTest rerun complete\n")


def main(num_threads, test_runner_name, results_formatter):
    """Run dotest.py in inferior mode in parallel.

    @param num_threads the parsed value of the num-threads command line
    argument.

    @param test_subdir optionally specifies a subdir to limit testing
    within.  May be None if the entire test tree is to be used.  This subdir
    is assumed to be relative to the lldb/test root of the test hierarchy.

    @param test_runner_name if specified, contains the test runner
    name which selects the strategy used to run the isolated and
    optionally concurrent test runner. Specify None to allow the
    system to choose the most appropriate test runner given desired
    thread count and OS type.

    @param results_formatter if specified, provides the TestResultsFormatter
    instance that will format and output test result data from the
    side-channel test results.  When specified, inferior dotest calls
    will send test results side-channel data over a socket to the parallel
    test runner, which will forward them on to results_formatter.
    """

    # Do not shut down on sighup.
    if hasattr(signal, 'SIGHUP'):
        signal.signal(signal.SIGHUP, signal.SIG_IGN)

    dotest_argv = sys.argv[1:]

    global RESULTS_FORMATTER
    RESULTS_FORMATTER = results_formatter

    # We can't use sys.path[0] to determine the script directory
    # because it doesn't work under a debugger
    parser = dotest_args.create_parser()
    global dotest_options
    dotest_options = dotest_args.parse_args(parser, dotest_argv)

    adjust_inferior_options(dotest_argv)

    session_dir = os.path.join(os.getcwd(), dotest_options.s)

    test_subdir = configuration.get_absolute_path_to_root_test_dir()

    # clean core files in test tree from previous runs (Linux)
    cores = find('core.*', test_subdir)
    for core in cores:
        os.unlink(core)

    system_info = " ".join(platform.uname())

    # Figure out which test files should be enabled for expected
    # timeout
    expected_timeout = getExpectedTimeouts(dotest_options.lldb_platform_name)
    if results_formatter is not None:
        results_formatter.set_expected_timeouts_by_basename(expected_timeout)

    # Setup the test runner context.  This is a dictionary of information that
    # will be passed along to the timeout pre-kill handler and allows for loose
    # coupling of its implementation.
    runner_context = {
        "arch": configuration.arch,
        "platform_name": configuration.lldb_platform_name,
        "platform_url": configuration.lldb_platform_url,
        "platform_working_dir": configuration.lldb_platform_working_dir,
    }

    # Figure out which testrunner strategy we'll use.
    runner_strategies_by_name = get_test_runner_strategies(
        num_threads, session_dir, runner_context)

    # If the user didn't specify a test runner strategy, determine
    # the default now based on number of threads and OS type.
    if not test_runner_name:
        test_runner_name = default_test_runner_name(num_threads)

    if test_runner_name not in runner_strategies_by_name:
        raise Exception(
            "specified testrunner name '{}' unknown. Valid choices: {}".format(
                test_runner_name,
                list(runner_strategies_by_name.keys())))
    test_runner_func = runner_strategies_by_name[test_runner_name]

    # Do the first test run phase.
    summary_results = walk_and_invoke(
        find_test_files_in_dir_tree(test_subdir),
        dotest_argv,
        num_threads,
        test_runner_func)

    (timed_out, passed, failed, unexpected_successes, pass_count,
     fail_count) = summary_results

    # Check if we have any tests to rerun as phase 2.
    if results_formatter is not None:
        tests_for_rerun = results_formatter.tests_for_rerun
        results_formatter.tests_for_rerun = {}

        if tests_for_rerun is not None and len(tests_for_rerun) > 0:
            rerun_file_count = len(tests_for_rerun)
            print("\n{} test files marked for rerun\n".format(
                rerun_file_count))

            # Clear errors charged to any of the files of the tests that
            # we are rerunning.
            # https://llvm.org/bugs/show_bug.cgi?id=27423
            results_formatter.clear_file_level_issues(tests_for_rerun,
                                                      sys.stdout)

            # Check if the number of files exceeds the max cutoff.  If so,
            # we skip the rerun step.
            if rerun_file_count > configuration.rerun_max_file_threshold:
                print("Skipping rerun: max rerun file threshold ({}) "
                      "exceeded".format(
                          configuration.rerun_max_file_threshold))
            else:
                rerun_tests(test_subdir, tests_for_rerun, dotest_argv,
                            session_dir, runner_context)

    # The results formatter - if present - is done now.  Tell it to
    # terminate.
    if results_formatter is not None:
        results_formatter.send_terminate_as_needed()

    timed_out = set(timed_out)
    num_test_files = len(passed) + len(failed)
    num_test_cases = pass_count + fail_count

    # move core files into session dir
    cores = find('core.*', test_subdir)
    for core in cores:
        dst = core.replace(test_subdir, "")[1:]
        dst = dst.replace(os.path.sep, "-")
        os.rename(core, os.path.join(session_dir, dst))

    # remove expected timeouts from failures
    for xtime in expected_timeout:
        if xtime in timed_out:
            timed_out.remove(xtime)
            failed.remove(xtime)
            result = "ExpectedTimeout"
        elif xtime in passed:
            result = "UnexpectedCompletion"
        else:
            result = None  # failed

        if result:
            test_name = os.path.splitext(xtime)[0]
            touch(os.path.join(session_dir, "{}-{}".format(result, test_name)))

    # Only run the old summary logic if we don't have a results formatter
    # that already prints the summary.
    print_legacy_summary = results_formatter is None
    if not print_legacy_summary:
        # Print summary results.  Summarized results at the end always
        # get printed to stdout, even if --results-file specifies a different
        # file for, say, xUnit output.
        results_formatter.print_results(sys.stdout)

        # Figure out exit code by count of test result types.
        issue_count = 0
        for issue_status in EventBuilder.TESTRUN_ERROR_STATUS_VALUES:
            issue_count += results_formatter.counts_by_test_result_status(
                issue_status)

        # Return with appropriate result code
        if issue_count > 0:
            sys.exit(1)
        else:
            sys.exit(0)
    else:
        # Print the legacy test results summary.
        print()
        sys.stdout.write("Ran %d test suites" % num_test_files)
        if num_test_files > 0:
            sys.stdout.write(" (%d failed) (%f%%)" % (
                len(failed), 100.0 * len(failed) / num_test_files))
        print()
        sys.stdout.write("Ran %d test cases" % num_test_cases)
        if num_test_cases > 0:
            sys.stdout.write(" (%d failed) (%f%%)" % (
                fail_count, 100.0 * fail_count / num_test_cases))
        print()
        exit_code = 0

        if len(failed) > 0:
            failed.sort()
            print("Failing Tests (%d)" % len(failed))
            for f in failed:
                print("%s: LLDB (suite) :: %s (%s)" % (
                    "TIMEOUT" if f in timed_out else "FAIL", f, system_info
                ))
            exit_code = 1

        if len(unexpected_successes) > 0:
            unexpected_successes.sort()
            print("\nUnexpected Successes (%d)" % len(unexpected_successes))
            for u in unexpected_successes:
                print(
                    "UNEXPECTED SUCCESS: LLDB (suite) :: %s (%s)" %
                    (u, system_info))

    sys.exit(exit_code)

if __name__ == '__main__':
    sys.stderr.write(
        "error: dosep.py no longer supports being called directly. "
        "Please call dotest.py directly.  The dosep.py-specific arguments "
        "have been added under the Parallel processing arguments.\n")
    sys.exit(128)
