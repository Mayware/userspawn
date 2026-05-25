# userspawn
userspawn runs an executable, you specify, when a user logs in. This could be a bash script, or anything else you want!
This can be used to launch a user instance of your init system.

## Installation & Usage
Install userspawn from one of the repos listed below:

[Arch User Repository](https://aur.archlinux.org/packages/userspawn-git)

Create the relevant startup script. Keep in mind, userspawn must be ran as root.
For example, for dinit, place the following in `/usr/lib/dinit.d/userspawn`:

```bash
type = process
command = /usr/bin/userspawn
depends-on = login.target
restart = true
log-type = buffer
log-buffer-size = 1000000 # 1 MB
```

Following this, create `~/.userspawnrc` (i.e. in your home directory), and add what you need.
Alternatively, you can also create it at `~/.config/userspawn/userspawnrc`
For a default system-wide config, you can also create `/etc/xdg/userspawn/userspawnrc`.
For example, for dinit, you may want:

```bash
#!/bin/bash
exec dinit --user
```

Remember to `chmod +x .userspawnrc`.

Now, enable the service you previously created, and it should be working!
If something goes wrong, try running the program manually as root in another shell, to see what is going wrong.
You can also compile in debug mode, to get additional debug messages.

## How it works
userspawn works in conjunction with (e)logind, and hence, dbus. It listens for users being added or removed.
For every user addition, a new cgroup is created for that user and `.userspawnrc` is spawned into that cgroup.
Upon user removal, that cgroup is then cleaned up, as triggered by (e)logind.

## Misc
If you encounter any issues, feel free to make a github issue. Similarly such for feature requests, although the scope is limited.

Licensed under `LGPL-3.0-or-later`, with the `LGPL-3.0 Linking Exception`.

