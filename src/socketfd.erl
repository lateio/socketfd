% ------------------------------------------------------------------------------
%
% Copyright © 2019, Lauri Moisio <l@arv.io>
%
% The ISC License
%
% Permission to use, copy, modify, and/or distribute this software for any
% purpose with or without fee is hereby granted, provided that the above
% copyright notice and this permission notice appear in all copies.
%
% THE SOFTWARE IS PROVIDED “AS IS” AND THE AUTHOR DISCLAIMS ALL WARRANTIES
% WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
% MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
% ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
% WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
% ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
% OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
%
% ------------------------------------------------------------------------------
%
% This module - and the NIFs associated with it - enable the allocation
% and binding of raw socket fds, which can later on be used with gen_udp and such.
%
% Note: most NIFs can return POSIX error details. Typically part of the reason
%       for using Erlang is presumably to escape the need to painstakingly check
%       for every error. Thus when using this module, please stick to the rule
%       of minimal local error handling, even though detailed error reasons
%       are available.
-module(socketfd).
-export([
    open/1,
    open/2,
    open/3,
    get/1,
    dup/1,
    close/1,
    address_family/1,
    socket_type/1,
    address/1,
    port/1,
    address_port/1
]).

-on_load(load_nifs/0).

-opaque socketfd() :: identifier().
-type socket_type() ::
      'tcp'
    | 'stream'
    | 'udp'
    | 'datagram'.
-type socket_family() :: 'inet' | 'inet6' | 'any'.

-export_type([socketfd/0]).


load_nifs() ->
    case code:priv_dir(socketfd) of
        {error, bad_name} -> error(bad_appname);
        Dir -> erlang:load_nif(filename:join(Dir, socketfd), 0)
    end.


-spec open(socket_type())
    -> {'ok', socketfd()}
     | {'error', term()}.
open(Socket) -> open(Socket, 0).

-spec open(socket_type(), inet:port_number())
    -> {'ok', socketfd()}
     | {'error', term()}.
open(Socket, Port) -> open(Socket, Port, any).

-spec open(
    socket_type(),
    inet:port_number(),
    socket_family() | inet:ip_address()
)
    -> {'ok', socketfd()}
     | {'error', term()}.
open(_, _, _) -> {error, nifs_not_loaded}.


-spec dup(socketfd())
    -> {'ok', socketfd()}
     | {'error', Reason :: term()}.
dup(_) -> {error, nifs_not_loaded}.


-spec get(socketfd())
    -> integer()
     | {'error', Reason :: term()}.
get(_) -> {error, nifs_not_loaded}.


-spec close(socketfd())
    -> 'ok'
     | {'error', 'nifs_not_loaded'}.
close(_) -> {error, nifs_not_loaded}.


-spec address_family(socketfd())
    -> 'inet'
     | 'inet6'
     | {'error', Reason :: term()}.
address_family(_) -> {error, nifs_not_loaded}.


-spec socket_type(socketfd())
    -> 'tcp'
     | 'udp'
     | {'error', Reason :: term()}.
socket_type(_) -> error(nifs_not_loaded).


-spec address(socketfd())
    -> inet:ip_address()
     | {'error', Reason :: term()}.
address(Fd) ->
    case address_port(Fd) of
        {error, _}=Tuple -> Tuple;
        AddressPort -> element(1, AddressPort)
    end.


-spec port(socketfd())
    -> inet:port_number()
     | {'error', Reason :: term()}.
port(Fd) ->
    case address_port(Fd) of
        {error, _}=Tuple -> Tuple;
        AddressPort -> element(2, AddressPort)
    end.


-spec address_port(socketfd())
    -> {inet:ip_address(), inet:port_number()}
     | {'error', Reason :: term()}.
address_port(_) -> {error, nifs_not_loaded}.
