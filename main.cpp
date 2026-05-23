#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dbus/dbus.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/sched.h>
#include <pwd.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <unistd.h>
import std;

#ifdef NDEBUG
constexpr bool debug = false;
#else
constexpr bool debug = true;
#endif

template<typename... Arguments>
void debug_print(std::format_string<Arguments...> to_format, Arguments&&... arguments) {
	if constexpr (debug) {
		std::println("[DEBUG] {}", std::format(to_format, std::forward<Arguments>(arguments)...));
	}
}

void get_error(DBusError& error) {
	std::println("[DBUS ERROR] {} {}", error.name, error.message);
	dbus_error_free(&error);
}

void check_fail_error(DBusError& error) {
	if (dbus_error_is_set(&error)) {
		get_error(error);
		std::exit(1);
	}
}

std::tuple<int, const char*> get_user(DBusMessage* message) {
	debug_print("Getting user from dbus message");
	DBusMessageIter iter;
	dbus_message_iter_init(message, &iter);
	debug_print("Initialised iter");

	dbus_uint32_t uid;
	const char* path;

	dbus_message_iter_get_basic(&iter, &uid);
	dbus_message_iter_next(&iter);
	debug_print("Got uid");
	dbus_message_iter_get_basic(&iter, &path);
	debug_print("Got path");

	return std::tuple(uid, path);
}

std::filesystem::path get_cgroup(dbus_uint32_t uid) {
	return std::format("/sys/fs/cgroup/user-{}.userspawn", uid);
}

void end_cgroup(const std::filesystem::path& cgroup) {
	debug_print("Ending cgroup {}", cgroup.c_str());
	// Just a data dump we can write to; see usage later on for clarification
	static char dump[sizeof(inotify_event) + NAME_MAX + 1];

	// Tell the cgroup to kys
	debug_print("Killing cgroup");
	std::ofstream kill_file(cgroup / "cgroup.kill");
	if (!kill_file) {
		std::println("Failed to open cgroup kill file! Unable to cleanup! {}, {}", cgroup.string(), strerror(errno));
		return;
	}
	kill_file << "1";
	kill_file.flush();
	if (!kill_file) {
		std::println("Failed to write to cgroup kill file! Unable to cleanup! {}, {}", cgroup.string(), strerror(errno));
		return;
	}
	kill_file.close();
	debug_print("Wrote kill");

	// Yield until the cgroup's processes are cleaned up
	// populated 0 will appear in the file, when it has finished
	// Pretty good resource for inotify: https://developer.ibm.com/tutorials/l-inotify/
	auto events = cgroup / "cgroup.events";
	int fd = inotify_init();
	inotify_add_watch(fd, events.c_str(), IN_MODIFY);
	debug_print("Added watcher");

	while (true) {
		// Check if populated 0 is set
		std::ifstream file(events);
		std::string line;
		bool empty = false;
		while (std::getline(file, line)) {
			// The populated field is recursive, i.e, it will only be 0 when all the child cgroups are too
			// https://www.kernel.org/doc/html/v4.18/admin-guide/cgroup-v2.html#un-populated-notification
			if (line == "populated 0") {
				debug_print("No more children");
				goto done;
			}
		}

		// Blocks until cgroup.events is changed, hence this isn't a busy loop
		// We don't need this data, but we need to read it into a buffer, for correct yielding
		// https://man7.org/linux/man-pages/man7/inotify.7.html
		debug_print("Still has children, yielding");
		read(fd, dump, sizeof(dump));
	}
done:
	close(fd);
	debug_print("Closed inotify fd");

	// Cgroups can only be removed, if they have no child cgroups
	// Therefore, we must remove them bottom up
	// https://man7.org/linux/man-pages/man7/cgroups.7.html
	std::vector<std::filesystem::path> cgroups;
	for (auto& entry : std::filesystem::recursive_directory_iterator(cgroup)) {
		if (entry.is_directory()) {
			cgroups.push_back(entry.path());
		}
	}
	std::ranges::reverse(cgroups);
	for (auto& subcgroup : cgroups) {
		debug_print("Removing subcgroup {}", subcgroup.c_str());
		std::filesystem::remove(subcgroup);
	}
	std::filesystem::remove(cgroup);
	debug_print("Removed primary cgroup {}", cgroup.c_str());
}

void start_user(dbus_uint32_t uid) {
	auto cgroup = get_cgroup(uid);
	if (std::filesystem::exists(cgroup)) {
		std::println("[WARNING] A start user request was received, but the cgroup already exists. "
					 "This should never occur, perhaps a cleanup was unable to run before? It will be regenerated.");
		end_cgroup(cgroup);
	}

	// Usually this is called pw,but that doesn't really make sense anymore
	passwd* entry = getpwuid(uid);
	debug_print("Got entry (pw)");
	auto script_path = std::string(entry->pw_dir) + "/.userspawnrc";
	if (!std::filesystem::exists(script_path)) {
		script_path = std::string(entry->pw_dir) + "/.config/userspawn/userspawnrc";
		if (!std::filesystem::exists(script_path)) {
			std::println("[ERROR] The path {} doesn't exist - please create "
						 " .userspawnrc and specify what you want launched!",
				script_path);
			return;
		}
	}

	// Create the env vars
	std::string home_env = std::format("HOME={}", entry->pw_dir);
	std::string user_env = std::format("USER={}", entry->pw_name);
	std::string logname_env = std::format("LOGNAME={}", entry->pw_name);
	std::string shell_env = std::format("SHELL={}", entry->pw_shell);
	std::string path_env = "PATH=/usr/local/sbin:/usr/local/bin:/usr/bin";
	std::string runtime_env = std::format("XDG_RUNTIME_DIR=/run/user/{}", uid);
	debug_print("Got env vars");

	const char* env[] = {
		home_env.c_str(),
		user_env.c_str(),
		logname_env.c_str(),
		shell_env.c_str(),
		path_env.c_str(),
		runtime_env.c_str(),
		nullptr,
	};

	std::filesystem::create_directory(cgroup);
	if (chown(cgroup.c_str(), entry->pw_uid, entry->pw_gid) != 0) {
		std::println("[ERROR] Failed to chown cgroup {}: {}", cgroup.c_str(), strerror(errno));
		return;
	}
	int cgroup_fd = open(cgroup.c_str(), O_RDONLY | O_DIRECTORY);
	debug_print("Created, chown'ed and opened cgroup");

	// Clone directly into the cgroup
	clone_args args = {};
	args.flags = CLONE_INTO_CGROUP;
	args.cgroup = cgroup_fd;
	// Should already be 0-init, but just making this explicit
	// as quoted 'If no signal (i.e., zero) is specified, then the
	// parent process is not signaled when the child terminates.'
	// This shouldn't(?) matter anyway, since we set sigchild to sig_ign,
	// but can't hurt
	args.exit_signal = 0;
	debug_print("About to syscall clone into");

	// https://man7.org/linux/man-pages/man2/clone.2.html
	pid_t pid = syscall(SYS_clone3, &args, sizeof(args));
	if (pid == 0) {
		// Drop permissions
		// Also gives supplementary groups, beyond just the primary
		if (initgroups(entry->pw_name, entry->pw_gid) != 0) {
			write(STDERR_FILENO, "Initgroups failed\n", 18);
			_exit(1);
		}
		if (setgid(entry->pw_gid) != 0) {
			write(STDERR_FILENO, "Setgid failed\n", 14);
			_exit(1);
		}
		if (setuid(entry->pw_uid) != 0) {
			write(STDERR_FILENO, "Setuid failed\n", 14);
			_exit(1);
		}
		if (geteuid() != entry->pw_uid || getegid() != entry->pw_gid) {
			write(STDERR_FILENO, "Failed to drop privileges\n", 26);
			_exit(1);
		}

		// Actually execute their script
		execle(script_path.c_str(), "userspawnrc", nullptr, env);

		// Will only run, if execl somehow fails
		write(STDERR_FILENO, "Failed to exec user", 15);
		write(STDERR_FILENO, script_path.c_str(), script_path.size());
		write(STDERR_FILENO, "! Make sure it's chmod +x'ed: ", 30);
		// https://man7.org/linux/man-pages/man3/strerror.3.html, says its thread safe
		const char* error = strerrordesc_np(errno);
		write(STDERR_FILENO, error, strlen(error));
		write(STDERR_FILENO, "\n", 1);
		_exit(1);
	} else if (pid == -1) {
		std::println("[ERROR] Clone3 failed: {}", strerror(errno));
		return;
	}
	close(cgroup_fd);
	std::println("[LOG] Started userspawnrc for user {}", uid);
}

void end_user(dbus_uint32_t uid) {
	auto cgroup = get_cgroup(uid);
	if (std::filesystem::exists(cgroup)) {
		end_cgroup(cgroup);
	} else {
		std::println("[LOG] Not cleaning up cgroup {} for uid {}, as it didn't exist!", cgroup.c_str(), uid);
	}
	std::println("[LOG] Cleaned up {} for uid {}", cgroup.c_str(), uid);
}

int main() {
	if (geteuid() != 0) {
		std::println("Userspawn must be ran as root!");
		std::exit(1);
	}

	// Abandon our offspring (https://stackoverflow.com/a/7171836)
	// Essentially, this means our zombies are auto-reaped
	signal(SIGCHLD, SIG_IGN);

	// Not many examples of using libdbus itself, but this was
	// good; http://www.matthew.ath.cx/articles/dbus
	DBusError error;
	DBusMessage* message;
	DBusConnection* connection;
	dbus_error_init(&error);

	// Initialise the connection (note, the bus is shared, do not close it)
	connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);

	// dbus will terminate our program, if it disconnects
	// Alternatively, we could monitor disconnects and restart, but this should be running
	// under a service manager, which should restart us, so it'd just be duplication
	if (connection == nullptr) {
		std::println("[ERROR] Failed to initialise dbus connection");
		get_error(error);
		return 1;
	}
	dbus_connection_set_exit_on_disconnect(connection, true);

	// Set initial listens for users being added or removed (not sessions, but users)
	dbus_bus_add_match(connection, "type='signal',interface='org.freedesktop.login1.Manager',member='UserNew'", &error);
	check_fail_error(error);
	dbus_bus_add_match(connection, "type='signal',interface='org.freedesktop.login1.Manager',member='UserRemoved'", &error);
	check_fail_error(error);
	dbus_connection_flush(connection);

	// Get the initial logged in users - this shouldn't do anything in most cases
	// Only if we're started after users are already logged in, for some reason
	// For endpoint docs, see: https://man7.org/linux/man-pages/man5/org.freedesktop.login1.5.html
	message = dbus_message_new_method_call(
		"org.freedesktop.login1",
		"/org/freedesktop/login1",
		"org.freedesktop.login1.Manager",
		"ListUsers");
	if (message == nullptr) {
		std::println("[ERROR] Failed to get initial user-list");
		get_error(error);
		return 1;
	}

	// -1 means use default timeout
	DBusMessage* reply = dbus_connection_send_with_reply_and_block(connection, message, -1, &error);
	dbus_message_unref(message);
	if (reply == nullptr) {
		std::println("Failed to get dbus response, for initial user list");
		get_error(error);
		return 1;
	}

	// I do not assert the validity of the received reply, if (e)logind gives an unexpected
	// response, everything is cooked anyway
	DBusMessageIter iter, array_iter, struct_iter;
	dbus_message_iter_init(reply, &iter);
	dbus_message_iter_recurse(&iter, &array_iter);

	while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID) {
		dbus_message_iter_recurse(&array_iter, &struct_iter);

		dbus_uint32_t uid;
		// const char* username;
		// const char* path;

		dbus_message_iter_get_basic(&struct_iter, &uid);
		dbus_message_iter_next(&struct_iter);
		// dbus_message_iter_get_basic(&struct_iter, &username);
		// dbus_message_iter_next(&struct_iter);
		// dbus_message_iter_get_basic(&struct_iter, &path);

		dbus_message_iter_next(&array_iter);
		start_user(uid);
	}
	dbus_message_unref(reply);

	// Poll the matches we set up, given we've just handled any existing ones.
	// This is where the bulk of userspawns should actually happen
	// Note, we manually pump dbus (-1 means indefinite block), rather than doing its
	// callback / automatic dispatch appraoch, since I don't like my event loop being stolen
	// We'll never see the end of this loop anyway, since exit on bus disconnection is on
	while (dbus_connection_read_write(connection, -1)) {

		// Multiple messages may arrive at once, so parse them all, until there is no more
		// WIthout this, we just receive the name acquired event, then do nothing
		debug_print("Received a dbus message");
		while (true) {
			// https://dbus.freedesktop.org/doc/api/html/group__DBusConnection.html#ga1e40d994ea162ce767e78de1c4988566
			DBusMessage* message = dbus_connection_pop_message(connection);
			if (message == nullptr) {
				break;
			}

			bool should_remove = false;
			if (dbus_message_is_signal(message, "org.freedesktop.login1.Manager", "UserRemoved")) {
				should_remove = true;
			} else if (!dbus_message_is_signal(message, "org.freedesktop.login1.Manager", "UserNew")) {
				debug_print("Skipping message: type={} interface={} member={}",
					dbus_message_get_type(message),
					dbus_message_get_interface(message) ?: "null",
					dbus_message_get_member(message) ?: "null");
				dbus_message_unref(message);
				continue;
			}

			debug_print("Should remove was {}", should_remove);
			auto user = get_user(message);

			if (should_remove) {
				end_user(std::get<0>(user));
			} else {
				start_user(std::get<0>(user));
			}
			dbus_message_unref(message);
		}
	}
}
