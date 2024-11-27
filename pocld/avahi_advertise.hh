/* avahi_advertise.hh - part of pocl-daemon that performs mDNS on local network
 to advertise the remote server and its devices.


   Copyright (c) 2023-2024 Yashvardhan Agarwal / Tampere University

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

#include <CL/cl.h>
#include <string>

#include <avahi-client/publish.h>
#include <avahi-common/thread-watch.h>

typedef struct serverInfo_t {
  std::string id;   // Unique ID with which server advertises itself
  cl_int ifIndex;   // Interface index to use to advertise the server
  cl_int ipProto;   // IPv4 or IPv6 or both
  uint16_t port;    // Port used by the server
  std::string info; // Server's device info
} serverInfo;

// Class handling Avahi advertisements.
// Manages the Avahi client, entry group, and threaded poll,
// enabling a server to advertise itself as an mDNS service on the network.
class AvahiAdvertise {
public:
  ~AvahiAdvertise();

  // Launches Avahi advertisement with specified server details.
  void launchAvahiAdvertisement(std::string serverID, cl_int ifIndex,
                                cl_int ipProto, uint16_t port,
                                std::string info);
  // Cleans up Avahi resources, including client and entry group. Also stops
  // running avahi thread and clears its resources.
  void clearAvahi();

private:
  // Adds the server as a service advertised using mDNS by the host.
  static void createService(AvahiClient *c, void *userdata);
  // Called when avahi client state changes.
  static void avahiClientCallback(AvahiClient *c, AvahiClientState state,
                                  AVAHI_GCC_UNUSED void *userdata);
  // Called whenever the entry group's state changes.
  static void entryGroupCallback(AvahiEntryGroup *g, AvahiEntryGroupState state,
                                 AVAHI_GCC_UNUSED void *userdata);

  serverInfo server;
  AvahiEntryGroup *avahiEntryGroup = NULL;
  AvahiThreadedPoll *avahiThreadedPoll = NULL;
  AvahiClient *avahiClient = NULL;
};