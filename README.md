socketfd
=====

"NIFs for creating TCP/UDP socket fds in Erlang"

Why
----

You might have encountered the following case:

```Erlang
{ok, Socket} = gen_udp:open(0, [{active, false}]),
Fn = fun () -> {ok, _} = gen_udp:recv(Socket, 1500) end,
Pid1 = spawn_link(Fn),
Pid2 = spawn_link(Fn),
Pid3 = spawn_link(Fn).
```

Last two spawned processes will crash with `badmatch` errors because, unlike with gen_tcp listen sockets, it is not possible for multiple processes to wait for input on a single socket.

Still wanting to have multiple processes accepting messages on the same socket, I came up with the following solution:

```Erlang
{ok, Fd} = socketfd:open(udp, 0, inet),
Fn = fun () ->
    {ok, FunFd} = socketfd:dup(Fd),
    {ok, FunSocket} = gen_udp:open(0, [{fd, socketfd:get(FunFd)}, inet, {active, false}]),
    {ok, _} = gen_udp:recv(FunSocket, 1500)
end,
[spawn(Fn) || _ <- lists:seq(1, 5)].
```

By creating and binding the socket directly and then duplicating it with [dup()](https://linux.die.net/man/2/dup), it is possible to have multiple workers waiting on the same socket. Whether or not this is beneficial in terms of performance is an open question, I do not have numbers proving the case either way.

Caution
----

Creating the sockets directly leaves us in charge of their lifetime. socketfd wraps the socket file descriptors in resources, which guarantees that at some point all the allocated sockets will be closed as a result of garbage collection. That, however, might take some time and can thus starve the Erlang vm of file descriptors when improperly used.

Direct access to file descriptors also allows other errors to occur:

```Erlang
{ok, Fd} = socketfd:open(udp, 0, inet),
Fn = fun () ->
    {ok, FunSocket} = gen_udp:open(0, [{fd, socketfd:get(Fd)}, inet, {active, false}]),
    {ok, _} = gen_udp:recv(FunSocket, 1500)
end,
Pid1 = spawn_link(Fn),
Pid2 = spawn_link(Fn).
```

In this example the spawned processes will intermittently steal control of the socket fd in Erlang vm's polling, producing rare error messages and confusion, not to mention losing the duplicate socket access for which we were using socketfd in the first place.

Should I use socketfd?
----

Almost certainly not. It is probably better to design your application in such a way that a UDP socket feeds a pool of workers, rather than duplicating the socket in hackish manner for each worker: this affords you more control of the message queue, as the pending messages can be sent to workers rather than be left to the OS and chances are that anything you are doing to the messages is more likely to the be the bottleneck rather than just the simple task of receiving and forwarding the messages.

However, if you need a quick refresh of custom resource types in NIFs, socketfd has an example of that.
