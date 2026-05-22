#include <cassert>
#include <dbus/dbus.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/inotify.h>
#include <unistd.h>
import std;

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
	DBusMessageIter iter;
	dbus_message_iter_init(message, &iter);

	dbus_uint32_t uid;
	const char* path;

	dbus_message_iter_get_basic(&iter, &uid);
	dbus_message_iter_next(&iter);
	dbus_message_iter_get_basic(&iter, &path);
	dbus_message_unref(message);

	return std::tuple(uid, path);
}

std::filesystem::path get_cgroup(dbus_uint32_t uid) {
	return std::format("/sys/fs/cgroup/user-{}.userspawn", uid);
}

void end_cgroup(const std::filesystem::path& cgroup) {
	// Just a data dump we can write to; see usage later on for clarification
	static char dump[sizeof(inotify_event) + NAME_MAX + 1];

	// Tell the cgroup to kys
	std::ofstream(cgroup / "cgroup.kill") << "1";

	// Yield until the cgroup's processes are cleaned up
	// populated 0 will appear in the file, when it has finished
	// Pretty good resource for inotify: https://developer.ibm.com/tutorials/l-inotify/
	auto events = cgroup / "cgroup.events";
	int fd = inotify_init();
	inotify_add_watch(fd, events.c_str(), IN_MODIFY);

	while (true) {
		// Check if populated 0 is set
		std::ifstream file(events);
		std::string line;
		bool empty = false;
		while (std::getline(file, line)) {
			// The populated field is recursive, i.e, it will only be 0 when all the child cgroups are too
			// https://www.kernel.org/doc/html/v4.18/admin-guide/cgroup-v2.html#un-populated-notification
			if (line == "populated 0") {
				goto done;
			}
		}

		// Blocks until cgroup.events is changed, hence this isn't a busy loop
		// We don't need this data, but we need to read it into a buffer, for correct yielding
		// https://man7.org/linux/man-pages/man7/inotify.7.html
		read(fd, dump, sizeof(dump));
	}
done:
	close(fd);

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
		std::filesystem::remove(subcgroup);
	}
	std::filesystem::remove(cgroup);
}

void start_user(dbus_uint32_t uid) {
	auto cgroup = get_cgroup(uid);
	if (std::filesystem::exists(cgroup)) {
		std::println("[WARNING] A start user request was received, but the cgroup already exists. This should never occur, perhaps a cleanup was unable to run before? It will be regenerated.");
	}
}

void end_user(dbus_uint32_t uid) {
}

int main() {
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
		const char* username;
		const char* path;

		dbus_message_iter_get_basic(&struct_iter, &uid);
		dbus_message_iter_next(&struct_iter);
		dbus_message_iter_get_basic(&struct_iter, &username);
		dbus_message_iter_next(&struct_iter);
		dbus_message_iter_get_basic(&struct_iter, &path);
		std::println("uid={} username={} path={}", uid, username, path);

		dbus_message_iter_next(&array_iter);
	}
	dbus_message_unref(reply);

	// Poll the matches we set up, given we've just handled any existing ones.
	// This is where the bulk of userspawns should actually happen
	// Note, we manually pump dbus (-1 means indefinite block), rather than doing its
	// callback / automatic dispatch appraoch, since I don't like my event loop being stolen
	// We'll never see the end of this loop anyway, since exit on bus disconnection is on
	while (dbus_connection_read_write(connection, -1)) {
		// https://dbus.freedesktop.org/doc/api/html/group__DBusConnection.html#ga1e40d994ea162ce767e78de1c4988566
		DBusMessage* message = dbus_connection_pop_message(connection);
		if (message == nullptr) {
			continue;
		}

		bool should_remove = false;
		// Kept this to just show, we can assume if it wasn't removed, it was new, which is default
		// if (dbus_message_is_signal(message, "org.freedesktop.login1.Manager", "UserNew")) {
		if (dbus_message_is_signal(message, "org.freedesktop.login1.Manager", "UserRemoved")) {
			should_remove = true;
		}

		auto user = get_user(message);
	}
}
