# Start here

A plain-language tour of what this project is, where it is, and why. No iOS
protocol knowledge is assumed, and every term is defined where it first appears. The
other documents are written for someone who already knows the domain, so read this one
first.

## What this project is

This is a C++ library that talks to an iPhone or an iPad directly over a USB cable.
It is meant to be embedded in another C++ program, which then lists and transfers
files, installs and removes apps, and starts and stops them.

The important part is what it does **not** need. Apple's own tools and most other
projects start a helper program in the background, or depend on a daemon called
`usbmuxd`, or call a command-line tool. This library does none of that: it opens the
cable itself and speaks the phone's protocols on its own.

## The phone is a locked building

The easiest way to hold the whole thing in your head is to picture the phone as a
locked building:

- The **USB cable** is the front door. Opening it means finding the phone and
  claiming the right channel on the cable.
- The **front desk** is a service called `lockdownd`. It is where a visitor checks
  in before going anywhere else.
- **Pairing** is showing ID at the desk. The first time, the phone asks a human to
  tap *Trust This Computer?*; after that, a saved record gets the visitor in
  without asking again.
- **Services** are the rooms. The desk hands out a key (a numbered channel) for a
  room by name, and the visitor talks to that room over the cable.
- **Many conversations at once** are possible on one cable. The phone mixes them
  together, which is why the library has a layer that keeps each conversation apart.
  That mixing layer is called the **mux**.

## What is already built

The library can already do all of the everyday work:

- find the phone and open the cable;
- pair with it once, and remember the pairing;
- read the phone's model, iOS version, and unique id;
- list a folder, check a path, and copy a file to or from the phone;
- start a service by name and talk to it.

The app install and uninstall code is written too; on a newer iPhone it needs the
tunnel described in the next section, which the library now has.

## The wall we hit

Installing an app did not work on an iPhone running iOS 17 or later. The library
connected to the phone's app-installer service, sent the request, and the phone never
answered. Nothing on our side was broken; the phone simply moved that service somewhere
else.

On iOS 17, the app installer and the developer tools no longer sit behind the front
desk. They sit behind a **private tunnel** instead. Apple's tools build that tunnel,
and so do other open-source projects, but this library did not have one yet.

## What the tunnel is

The library asks the phone for a tunnel, and the phone answers with three things:

- an **address** (where the hidden place is),
- a **port** (a numbered door at that address),
- and a **size limit** for each chunk of data.

After that, the same connection becomes a pipe carrying **network packets**. Network
packets are the small envelopes the internet uses to move data: each one has a short
header that says where it is from, where it is going, and how big the rest of it is.

So to reach the app installer, the library has to act like a very small computer
network inside itself: wrap a request into packets, send them through the tunnel, and
unwrap the replies. It only ever opens a few outgoing connections, so it is a small
network, not a full one. This small network is the genuinely new and hard part.

## What was just built

The first, easy part of the tunnel work is done, and it is two translation pieces:

1. **The tunnel message.** Before the tunnel opens, the two sides exchange one
   specific message. The library can now build that message (a fixed word, a length, and
   then text) and read the reply, including the address, the port, and the size limit.

2. **Cutting the pipe into packets.** Once the tunnel is open, the phone sends one
   long stream of bytes with nothing between the packets. Each packet, however, starts
   with a header that says how long it is. The library can now read just enough bytes
   to hand back exactly one whole packet at a time.

Neither piece needs a real phone to check: the tests feed made-up data and compare
the result. That is what the new tests do.

## Where it is now

Since then, the rest of the tunnel is done:

1. **The handshake.** The library asks the phone for the tunnel, the phone answers
   with the address, the port, and the size limit, and the library reads them back.
   Proven on a real iPhone running iOS 18.7.8.
2. **The small network layer.** The library wraps a request in network packets, opens
   a connection through the tunnel to the numbered door, and the phone accepts it.
   Proven on a real iPhone running iOS 18.7.8.
3. **The message format.** After the handshake the tunnel speaks its own kind of
   message. The library can now build and read that message's fixed frame and the
   structured value inside it, checked without a phone.
4. **The service list.** The library asks the phone to list its services over the
   tunnel and reaches one by name. Proven on a real iPhone running iOS 18.7.8.

With the tunnel complete, app install and uninstall work on a newer iPhone: the
library uploads the app over the tunnel and asks the phone's installer to install it,
then removes it. Verified on a real iPhone running iOS 18.7.8.

One piece is left: **app control** (starting, checking, and closing an app). The
phone moved that behind a different format called `DTX`, which the library does not
speak yet.

## The words this project uses

| Word | What it means here |
| --- | --- |
| `usbmuxd`, `libimobiledevice` | Helper programs that talk to the phone. This project replaces them, so it does not need them. |
| mux | The phone's mixing layer that keeps many conversations apart on one cable. |
| `lockdownd` | The phone's front desk. Every other service is reached through it. |
| pairing | Showing ID once, so the phone trusts this computer afterwards. |
| TLS | The encryption the phone turns on after check-in. |
| `AFC` | The phone's file service: listing folders and copying files. |
| `installation_proxy` | The phone's app installer and uninstaller. |
| iOS 17+ | The iPhone software versions where the app installer moved behind the tunnel. |
| `RSD` | The phone's service list, reached through the tunnel. |
| CoreDevice | Apple's newer name for the developer services behind the tunnel. |
| `RemoteXPC` | The format the tunnel speaks after the handshake. |
| `DTX` | The format the developer tools speak, the next layer after `RemoteXPC`. |
| IPv6 | The kind of network packets the tunnel carries. |
| TCP | The standard way two computers send data reliably. The link speaks it over the tunnel. |
| JSON | A common text format for structured data, used by the tunnel handshake. |

## Where to read more

- [`01-objective.md`](01-objective.md) — what the library is for, and what it will not do.
- [`03-roadmap.md`](03-roadmap.md) — the plan, slice by slice, and what is done.
- [`05-usage.md`](05-usage.md) — copy-pasteable code for one feature at a time.
- [`10-coredevice-tunnel.md`](10-coredevice-tunnel.md) — the tunnel plan in technical terms.
