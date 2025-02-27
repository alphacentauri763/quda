#include <tune_quda.h>
#include <comm_quda.h>
#include <quda.h>     // for QUDA_VERSION_STRING
#include <timer.h>
#include <sys/stat.h> // for stat()
#include <fcntl.h>
#include <cfloat> // for FLT_MAX
#include <ctime>
#include <fstream>
#include <typeinfo>
#include <map>
#include <list>
#include <unistd.h>
#include <uint_to_char.h>
#include <target_device.h>

#include <deque>
#include <queue>
#include <functional>
#include <utility>
#include <json_helper.h>

#include <communicator_quda.h>

//#define LAUNCH_TIMER
extern char *gitversion;

namespace quda
{
  static TuneKey last_key;

  TuneKey getLastTuneKey() { return quda::last_key; }

  typedef std::map<TuneKey, TuneParam> map;

  struct TraceKey {

    TuneKey key;
    float time;

    long device_bytes;
    long pinned_bytes;
    long mapped_bytes;
    long host_bytes;

    TraceKey() {}

    TraceKey(const TuneKey &key, float time) :
      key(key),
      time(time),
      device_bytes(device_allocated_peak()),
      pinned_bytes(pinned_allocated_peak()),
      mapped_bytes(mapped_allocated_peak()),
      host_bytes(host_allocated_peak())
    {
    }

    TraceKey(const TraceKey &) = default;
    TraceKey(TraceKey &&) = default;
    TraceKey &operator=(const TraceKey &) = default;
    TraceKey &operator=(TraceKey &&) = default;
  };

  // linked list that is augmented each time we call a kernel
  static std::list<TraceKey> trace_list;
  static int enable_trace = 0;

  int traceEnabled()
  {
    static bool init = false;

    if (!init) {
      char *enable_trace_env = getenv("QUDA_ENABLE_TRACE");
      if (enable_trace_env) {
        if (strcmp(enable_trace_env, "1") == 0) {
          // only explicitly posted trace events are included
          enable_trace = 1;
        } else if (strcmp(enable_trace_env, "2") == 0) {
          // enable full kernel trace and posted trace events
          enable_trace = 2;
        }
      }
      init = true;
    }
    return enable_trace;
  }

  void postTrace_(const char *func, const char *file, int line)
  {
    if (traceEnabled() >= 1) {
      char aux[TuneKey::aux_n];
      strcpy(aux, file);
      strcat(aux, ":");
      char tmp[TuneKey::aux_n];
      i32toa(tmp, line);
      strcat(aux, tmp);
      TuneKey key("", func, aux);
      TraceKey trace_entry(key, 0.0);
      trace_list.push_back(trace_entry);
    }
  }

  static const std::string quda_hash = QUDA_HASH; // defined in lib/Makefile
  static std::string resource_path;
  static map tunecache;
  static map::iterator it;
  static size_t initial_cache_size = 0;

#define STR_(x) #x
#define STR(x) STR_(x)
  static const std::string quda_version
    = STR(QUDA_VERSION_MAJOR) "." STR(QUDA_VERSION_MINOR) "." STR(QUDA_VERSION_SUBMINOR);
#undef STR
#undef STR_

  /** tuning in progress? */
  static bool tuning = false;
  static bool candidatetuning = true;

  bool activeTuning() { return tuning; }

  static bool profile_count = true;

  void disableProfileCount() { profile_count = false; }
  void enableProfileCount() { profile_count = true; }

  const map &getTuneCache() { return tunecache; }

  /**
   * Deserialize tunecache from an istream, useful for reading a file or receiving from other nodes.
   */
  static void deserializeTuneCache(std::istream &in)
  {
    std::string line;
    std::stringstream ls;

    TuneKey key;
    TuneParam param;

    std::string v;
    std::string n;
    std::string a;

    int check;

    while (in.good()) {
      getline(in, line);
      if (!line.length()) continue; // skip blank lines (e.g., at end of file)
      ls.clear();
      ls.str(line);
      ls >> v >> n >> a >> param.block.x >> param.block.y >> param.block.z;
      check = snprintf(key.volume, key.volume_n, "%s", v.c_str());
      if (check < 0 || check >= key.volume_n) errorQuda("Error writing volume string (check = %d)", check);
      check = snprintf(key.name, key.name_n, "%s", n.c_str());
      if (check < 0 || check >= key.name_n) errorQuda("Error writing name string (check=%d)", check);
      check = snprintf(key.aux, key.aux_n, "%s", a.c_str());
      if (check < 0 || check >= key.aux_n) errorQuda("Error writing aux string (check=%d)", check);
      ls >> param.grid.x >> param.grid.y >> param.grid.z >> param.shared_bytes >> param.aux.x >> param.aux.y
        >> param.aux.z >> param.aux.w >> param.time;
      ls.ignore(1);               // throw away tab before comment
      getline(ls, param.comment); // assume anything remaining on the line is a comment
      param.comment += "\n";      // our convention is to include the newline, since ctime() likes to do this
      tunecache[key] = param;
    }
  }

  /**
   * Serialize tunecache to an ostream, useful for writing to a file or sending to other nodes.
   */
  static void serializeTuneCache(std::ostream &out)
  {
    map::iterator entry;

    for (entry = tunecache.begin(); entry != tunecache.end(); entry++) {
      TuneKey key = entry->first;
      TuneParam param = entry->second;

      out << std::setw(16) << key.volume << "\t" << key.name << "\t" << key.aux << "\t";
      out << param.block.x << "\t" << param.block.y << "\t" << param.block.z << "\t";
      out << param.grid.x << "\t" << param.grid.y << "\t" << param.grid.z << "\t";
      out << param.shared_bytes << "\t" << param.aux.x << "\t" << param.aux.y << "\t" << param.aux.z << "\t"
          << param.aux.w << "\t";
      out << param.time << "\t" << param.comment; // param.comment ends with a newline
    }
  }

  template <class T> struct less_significant {
    inline bool operator()(const T &lhs, const T &rhs)
    {
      return lhs.second.time * lhs.second.n_calls < rhs.second.time * rhs.second.n_calls;
    }
  };

  /**
   * Serialize tunecache to an ostream, useful for writing to a file or sending to other nodes.
   */
  static void serializeProfile(std::ostream &out, std::ostream &async_out)
  {
    map::iterator entry;
    double total_time = 0.0;
    double async_total_time = 0.0;

    // first let's sort the entries in decreasing order of significance
    typedef std::pair<TuneKey, TuneParam> profile_t;
    typedef std::priority_queue<profile_t, std::deque<profile_t>, less_significant<profile_t>> queue_t;
    queue_t q(tunecache.begin(), tunecache.end());

    // now compute total time spent in kernels so we can give each kernel a significance
    for (entry = tunecache.begin(); entry != tunecache.end(); entry++) {
      TuneKey key = entry->first;
      TuneParam param = entry->second;

      char tmp[TuneKey::aux_n] = {};
      strncpy(tmp, key.aux, TuneKey::aux_n);
      bool is_policy_kernel = strncmp(tmp, "policy_kernel", 13) == 0 ? true : false;
      bool is_policy = (strncmp(tmp, "policy", 6) == 0 && !is_policy_kernel) ? true : false;
      if (param.n_calls > 0 && !is_policy) total_time += param.n_calls * param.time;
      if (param.n_calls > 0 && is_policy) async_total_time += param.n_calls * param.time;
    }

    double cumulative_percent = 0;
    double cumulative_percent_async = 0;
    while (!q.empty()) {
      TuneKey key = q.top().first;
      TuneParam param = q.top().second;

      char tmp[TuneKey::aux_n] = {};
      strncpy(tmp, key.aux, TuneKey::aux_n);
      bool is_policy_kernel = strncmp(tmp, "policy_kernel", 13) == 0 ? true : false;
      bool is_policy = (strncmp(tmp, "policy", 6) == 0 && !is_policy_kernel) ? true : false;
      bool is_nested_policy = (strncmp(tmp, "nested_policy", 6) == 0) ? true : false; // nested policies not included

      // synchronous profile
      if (param.n_calls > 0 && !is_policy && !is_nested_policy) {
        double time = param.n_calls * param.time;
        double percent = 100 * time / total_time;
        cumulative_percent += percent;

        out << std::setw(12) << time << "\t";
        out << std::setw(12) << percent << "\t";
        out << std::setw(12) << cumulative_percent << "\t";
        out << std::setw(12) << param.n_calls << "\t";
        out << std::setw(12) << param.time << "\t";
        out << std::setw(16) << key.volume << "\t";
        out << key.name << "\t" << key.aux << "\t" << param.comment; // param.comment ends with a newline
      }

      // async policy profile
      if (param.n_calls > 0 && is_policy) {
        double time = param.n_calls * param.time;
        double percent = 100 * time / async_total_time;
        cumulative_percent_async += percent;

        async_out << std::setw(12) << time << "\t";
        async_out << std::setw(12) << percent << "\t";
        async_out << std::setw(12) << cumulative_percent_async << "\t";
        async_out << std::setw(12) << param.n_calls << "\t";
        async_out << std::setw(12) << param.time << "\t";
        async_out << std::setw(16) << key.volume << "\t";
        async_out << key.name << "\t" << key.aux << "\t" << param.comment; // param.comment ends with a newline
      }

      q.pop();
    }

    out << std::endl << "# Total time spent in kernels = " << total_time << " seconds" << std::endl;
    async_out << std::endl
              << "# Total time spent in asynchronous execution = " << async_total_time << " seconds" << std::endl;
  }

  /**
   * Serialize trace to an ostream, useful for writing to a file or sending to other nodes.
   */
  static void serializeTrace(std::ostream &out)
  {
    for (auto it = trace_list.begin(); it != trace_list.end(); it++) {

      TuneKey &key = it->key;

      // special case kernel members of a policy
      char tmp[TuneKey::aux_n] = {};
      strncpy(tmp, key.aux, TuneKey::aux_n);
      bool is_policy_kernel = strcmp(tmp, "policy_kernel") == 0 ? true : false;

      out << std::setw(12) << it->time << "\t";
      out << std::setw(12) << it->device_bytes << "\t";
      out << std::setw(12) << it->pinned_bytes << "\t";
      out << std::setw(12) << it->mapped_bytes << "\t";
      out << std::setw(12) << it->host_bytes << "\t";
      out << std::setw(16) << key.volume << "\t";
      if (is_policy_kernel) out << "\t";
      out << key.name << "\t";
      if (!is_policy_kernel) out << "\t";
      out << key.aux << std::endl;
    }
  }

  /**
   * Distribute the tunecache from node 0 to all other nodes.
   */
  static void broadcastTuneCache()
  {
    std::stringstream serialized;
    size_t size;

    if (comm_rank_global() == 0) {
      serializeTuneCache(serialized);
      size = serialized.str().length();
    }
    comm_broadcast_global(&size, sizeof(size_t));

    if (size > 0) {
      if (comm_rank_global() == 0) {
        comm_broadcast_global(const_cast<char *>(serialized.str().c_str()), size);
      } else {
        std::vector<char> serstr(size + 1);
        comm_broadcast_global(serstr.data(), size);
        serstr[size] = '\0'; // null-terminate
        serialized.str(serstr.data());
        deserializeTuneCache(serialized);
      }
    }
  }

  /*
   * Read tunecache from disk.
   */
  void loadTuneCache()
  {
    if (getTuning() == QUDA_TUNE_NO) {
      warningQuda("Autotuning disabled");
      return;
    }

    char *path;
    struct stat pstat;
    std::string cache_path, line, token;
    std::ifstream cache_file;
    std::stringstream ls;

    path = getenv("QUDA_RESOURCE_PATH");

    if (!path) {
      warningQuda("Environment variable QUDA_RESOURCE_PATH is not set.");
      warningQuda("Caching of tuned parameters will be disabled.");
      return;
    } else if (stat(path, &pstat) || !S_ISDIR(pstat.st_mode)) {
      warningQuda("The path \"%s\" specified by QUDA_RESOURCE_PATH does not exist or is not a directory.", path);
      warningQuda("Caching of tuned parameters will be disabled.");
      return;
    } else {
      resource_path = path;
    }

    bool version_check = true;
    char *override_version_env = getenv("QUDA_TUNE_VERSION_CHECK");
    if (override_version_env && strcmp(override_version_env, "0") == 0) {
      version_check = false;
      warningQuda("Disabling QUDA tunecache version check");
    }

    if (comm_rank_global() == 0) {
      cache_path = resource_path;
      cache_path += "/tunecache.tsv";
      cache_file.open(cache_path.c_str());

      if (cache_file) {

        if (!cache_file.good()) errorQuda("Bad format in %s", cache_path.c_str());
        getline(cache_file, line);
        ls.str(line);
        ls >> token;
        if (token.compare("tunecache")) errorQuda("Bad format in %s", cache_path.c_str());
        ls >> token;
        if (version_check && token.compare(quda_version))
          errorQuda("Cache file %s does not match current QUDA version. \nPlease delete this file or set the "
                    "QUDA_RESOURCE_PATH environment variable to point to a new path.",
                    cache_path.c_str());
        ls >> token;
#ifdef GITVERSION
        if (version_check && token.compare(gitversion))
          errorQuda("Cache file %s does not match current QUDA version. \nPlease delete this file or set the "
                    "QUDA_RESOURCE_PATH environment variable to point to a new path.",
                    cache_path.c_str());
#else
      if (version_check && token.compare(quda_version))
        errorQuda("Cache file %s does not match current QUDA version. \nPlease delete this file or set the "
                  "QUDA_RESOURCE_PATH environment variable to point to a new path.",
                  cache_path.c_str());
#endif
        ls >> token;
        if (version_check && token.compare(quda_hash))
          errorQuda("Cache file %s does not match current QUDA build. \nPlease delete this file or set the "
                    "QUDA_RESOURCE_PATH environment variable to point to a new path.",
                    cache_path.c_str());

        if (!cache_file.good()) errorQuda("Bad format in %s", cache_path.c_str());
        getline(cache_file, line); // eat the blank line

        if (!cache_file.good()) errorQuda("Bad format in %s", cache_path.c_str());
        getline(cache_file, line); // eat the description line

        deserializeTuneCache(cache_file);

        cache_file.close();
        initial_cache_size = tunecache.size();

        if (getVerbosity() >= QUDA_SUMMARIZE) {
          printfQuda("Loaded %d sets of cached parameters from %s\n", static_cast<int>(initial_cache_size),
                     cache_path.c_str());
        }

      } else {
        warningQuda("Cache file not found.  All kernels will be re-tuned (if tuning is enabled).");
      }
    }

    broadcastTuneCache();
  }

  /**
   * Write tunecache to disk.
   */
  void saveTuneCache(bool error)
  {
    time_t now;
    int lock_handle;
    std::string lock_path, cache_path;
    std::ofstream cache_file;

    if (resource_path.empty()) return;

      // FIXME: We should really check to see if any nodes have tuned a kernel that was not also tuned on node 0, since as things
      //       stand, the corresponding launch parameters would never get cached to disk in this situation.  This will come up if we
      //       ever support different subvolumes per GPU (as might be convenient for lattice volumes that don't divide evenly).

    if (comm_rank_global() == 0) {

      if (tunecache.size() == initial_cache_size && !error) return;

      // Acquire lock.  Note that this is only robust if the filesystem supports flock() semantics, which is true for
      // NFS on recent versions of linux but not Lustre by default (unless the filesystem was mounted with "-o flock").
      lock_path = resource_path + (error ? "/tunecache_error.lock" : "/tunecache.lock");
      lock_handle = open(lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
      if (lock_handle == -1) {
        warningQuda("Unable to lock cache file.  Tuned launch parameters will not be cached to disk.  "
                    "If you are certain that no other instances of QUDA are accessing this filesystem, "
                    "please manually remove %s",
                    lock_path.c_str());
        return;
      }
      char msg[] = "If no instances of applications using QUDA are running,\n"
                   "this lock file shouldn't be here and is safe to delete.";
      int stat = write(lock_handle, msg, sizeof(msg)); // check status to avoid compiler warning
      if (stat == -1) warningQuda("Unable to write to lock file for some bizarre reason");

      cache_path = resource_path + (error ? "/tunecache_error.tsv" : "/tunecache.tsv");
      cache_file.open(cache_path.c_str());

      if (getVerbosity() >= QUDA_SUMMARIZE) {
        printfQuda("Saving %d sets of cached parameters to %s\n", static_cast<int>(tunecache.size()), cache_path.c_str());
      }

      time(&now);
      cache_file << "tunecache\t" << quda_version;
#ifdef GITVERSION
      cache_file << "\t" << gitversion;
#else
    cache_file << "\t" << quda_version;
#endif
      cache_file << "\t" << quda_hash << "\t# Last updated " << ctime(&now) << std::endl;
      cache_file << std::setw(16) << "volume"
                 << "\tname\taux\tblock.x\tblock.y\tblock.z\tgrid.x\tgrid.y\tgrid.z\tshared_bytes\taux.x\taux.y\taux."
                    "z\taux.w\ttime\tcomment"
                 << std::endl;
      serializeTuneCache(cache_file);
      cache_file.close();

      // Release lock.
      close(lock_handle);
      remove(lock_path.c_str());

      initial_cache_size = tunecache.size();

    } else {
      // give process 0 time to write out its tunecache if needed, but
      // doesn't cause a hang if error is not triggered on process 0
      if (error) sleep(10);
    }
  }

  static bool policy_tuning = false;
  bool policyTuning() { return policy_tuning; }

  void setPolicyTuning(bool policy_tuning_) { policy_tuning = policy_tuning_; }

  static bool uber_tuning = false;
  bool uberTuning() { return uber_tuning; }

  void setUberTuning(bool uber_tuning_) { uber_tuning = uber_tuning_; }

  // flush profile, setting counts to zero
  void flushProfile()
  {
    for (map::iterator entry = tunecache.begin(); entry != tunecache.end(); entry++) {
      // set all n_calls = 0
      TuneParam &param = entry->second;
      param.n_calls = 0;
    }
  }

  // save profile
  void saveProfile(const std::string label)
  {
    time_t now;
    int lock_handle;
    std::string lock_path, profile_path, async_profile_path, trace_path;
    std::ofstream profile_file, async_profile_file, trace_file;

    if (resource_path.empty()) return;

    if (comm_rank_global() == 0) { // Make sure only one rank is writing to disk

      // Acquire lock.  Note that this is only robust if the filesystem supports flock() semantics, which is true for
      // NFS on recent versions of linux but not Lustre by default (unless the filesystem was mounted with "-o flock").
      lock_path = resource_path + "/profile.lock";
      lock_handle = open(lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
      if (lock_handle == -1) {
        warningQuda("Unable to lock profile file.  Profile will not be saved to disk.  "
                    "If you are certain that no other instances of QUDA are accessing this filesystem, "
                    "please manually remove %s",
                    lock_path.c_str());
        return;
      }
      char msg[] = "If no instances of applications using QUDA are running,\n"
                   "this lock file shouldn't be here and is safe to delete.";
      int stat = write(lock_handle, msg, sizeof(msg)); // check status to avoid compiler warning
      if (stat == -1) warningQuda("Unable to write to lock file for some bizarre reason");

      // profile counter for writing out unique profiles
      static int count = 0;

      char *profile_fname = getenv("QUDA_PROFILE_OUTPUT_BASE");

      if (!profile_fname) {
        warningQuda(
          "Environment variable QUDA_PROFILE_OUTPUT_BASE not set; writing to profile.tsv and profile_async.tsv");
        profile_path = resource_path + "/profile_" + std::to_string(count) + ".tsv";
        async_profile_path = resource_path + "/profile_async_" + std::to_string(count) + ".tsv";
        if (traceEnabled()) trace_path = resource_path + "/trace_" + std::to_string(count) + ".tsv";
      } else {
        profile_path = resource_path + "/" + profile_fname + "_" + std::to_string(count) + ".tsv";
        async_profile_path = resource_path + "/" + profile_fname + "_" + std::to_string(count) + "_async.tsv";
        if (traceEnabled())
          trace_path = resource_path + "/" + profile_fname + "_trace_" + std::to_string(count) + ".tsv";
      }

      count++;

      profile_file.open(profile_path.c_str());
      async_profile_file.open(async_profile_path.c_str());
      if (traceEnabled()) trace_file.open(trace_path.c_str());

      if (getVerbosity() >= QUDA_SUMMARIZE) {
        // compute number of non-zero entries that will be output in the profile
        int n_entry = 0;
        int n_policy = 0;
        for (map::iterator entry = tunecache.begin(); entry != tunecache.end(); entry++) {
          // if a policy entry, then we can ignore
          char tmp[TuneKey::aux_n] = {};
          strncpy(tmp, entry->first.aux, TuneKey::aux_n);
          TuneParam param = entry->second;
          bool is_policy = strcmp(tmp, "policy") == 0 ? true : false;
          if (param.n_calls > 0 && !is_policy) n_entry++;
          if (param.n_calls > 0 && is_policy) n_policy++;
        }

        printfQuda("Saving %d sets of cached parameters to %s\n", n_entry, profile_path.c_str());
        printfQuda("Saving %d sets of cached profiles to %s\n", n_policy, async_profile_path.c_str());
        if (traceEnabled())
          printfQuda("Saving trace list with %lu entries to %s\n", trace_list.size(), trace_path.c_str());
      }

      time(&now);

      std::string Label = label.empty() ? "profile" : label;

      profile_file << Label << "\t" << quda_version;
#ifdef GITVERSION
      profile_file << "\t" << gitversion;
#else
      profile_file << "\t" << quda_version;
#endif
      profile_file << "\t" << quda_hash << "\t# Last updated " << ctime(&now) << std::endl;
      profile_file << std::setw(12) << "total time"
                   << "\t" << std::setw(12) << "percent"
                   << "\t" << std::setw(12) << "cum. percent"
                   << "\t" << std::setw(12) << "calls"
                   << "\t" << std::setw(12) << "time / call"
                   << "\t" << std::setw(16) << "volume"
                   << "\tname\taux\tcomment" << std::endl;

      async_profile_file << Label << "\t" << quda_version;
#ifdef GITVERSION
      async_profile_file << "\t" << gitversion;
#else
      async_profile_file << "\t" << quda_version;
#endif
      async_profile_file << "\t" << quda_hash << "\t# Last updated " << ctime(&now) << std::endl;
      async_profile_file << std::setw(12) << "total time"
                         << "\t" << std::setw(12) << "percent"
                         << "\t" << std::setw(12) << "cum. percent"
                         << "\t" << std::setw(12) << "calls"
                         << "\t" << std::setw(12) << "time / call"
                         << "\t" << std::setw(16) << "volume"
                         << "\tname\taux\tcomment" << std::endl;

      serializeProfile(profile_file, async_profile_file);

      profile_file.close();
      async_profile_file.close();

      if (traceEnabled()) {
        trace_file << "trace"
                   << "\t" << quda_version;
#ifdef GITVERSION
        trace_file << "\t" << gitversion;
#else
      trace_file << "\t" << quda_version;
#endif
        trace_file << "\t" << quda_hash << "\t# Last updated " << ctime(&now) << std::endl;

        trace_file << std::setw(12) << "time\t" << std::setw(12) << "device-mem\t" << std::setw(12) << "pinned-mem\t";
        trace_file << std::setw(12) << "mapped-mem\t" << std::setw(12) << "host-mem\t";
        trace_file << std::setw(16) << "volume"
                   << "\tname\taux" << std::endl;

        serializeTrace(trace_file);

        trace_file.close();
      }

      // Release lock.
      close(lock_handle);
      remove(lock_path.c_str());
    }
  }

  TuneParam::TuneParam() :
    block(device::warp_size(), 1, 1),
    grid(1, 1, 1),
    shared_bytes(0),
    set_max_shared_bytes(false),
    aux(),
    time(FLT_MAX),
    n_calls(0)
  {
    aux = make_int4(1, 1, 1, 1);
  }

  int Tunable::blockStep() const { return device::warp_size(); }
  int Tunable::blockMin() const { return device::warp_size(); }

#ifdef LAUNCH_TIMER
  static TimeProfile launchTimer("tuneLaunch");
#endif

  /**
   * @brief Compare two TuneParams with respect to which has the lower time.
   *
   */
  struct TuneParamComp {
    bool operator()(const TuneParam &left, const TuneParam &right) const { return (left.time) < (right.time); }
  };
  /**
   * @brief Queue that stores the best tune parameters identified
   * in the 1st tuning phase which will be further tuned in the 2nd phase.
   *
   */

  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TuneParam, block, grid, shared_bytes, set_max_shared_bytes, aux, comment, time,
                                     n_calls)

  class TuneCandidates : public std::priority_queue<TuneParam, std::vector<TuneParam>, TuneParamComp>
  {
  private:
    const size_t max_size = 5;
    float besttime = FLT_MAX;

    /**
     * @brief Serialize tune candidates to json string.
     *
     * @return serialized json string
     */
    std::string serialize() const
    {
      auto p = std::make_pair(besttime, this->c);
      json j = p;
      return j.dump();
    }
    /**
     * @brief Deserialize tune candidates from json string.
     *
     * @param s
     */
    void deserialize(const std::string_view s)
    {
      auto j = json::parse(s);
      std::pair<float, decltype(this->c)> p = j;
      besttime = p.first;
      this->c = p.second;
    }

  public:
    /**
     * @brief Construct a new Tune Candidates object with specified size
     *
     * @param size number of candidates that will be kept for 2nd phase.
     */
    TuneCandidates(size_t size) : max_size(size) { }

    /**
     * @brief Push a new tuning candidate to the queue. Will be ignored if there are
     * already size faster ones.
     *
     * @param candidate
     */
    void pushCandidate(TuneParam candidate)
    {
      if (candidate.time < besttime) besttime = candidate.time;

      if (size() < max_size) {
        push(candidate);
        return;
      }
      if (candidate.time < top().time) {
        pop();
        push(candidate);
      }
    }

    /**
     * @brief Broadcast candidates among ranks to make sure policy tuning does not break.
     *
     */
    void broadcast()
    {
      size_t size;
      std::string serialized;
      if (comm_rank_global() == 0) {
        serialized = serialize();
        size = serialized.length();
      }
      comm_broadcast_global(&size, sizeof(size_t));

      if (size > 0) {
        if (comm_rank_global() == 0) {
          comm_broadcast_global(const_cast<char *>(serialized.c_str()), size);
        } else {
          std::vector<char> serstr(size + 1);
          comm_broadcast_global(serstr.data(), size);
          serstr[size] = '\0'; // null-terminate
          std::string_view deserialized(serstr.data());
          deserialize(deserialized);
        }
      }
    }

    /**
     * @brief Return the best time found in tuning.
     *
     * @return time in s
     */
    float getBestTime() const { return besttime; }
  };

  /**
   * Return the optimal launch parameters for a given kernel, either
   * by retrieving them from tunecache or autotuning on the spot.
   */
  TuneParam tuneLaunch(Tunable &tunable, QudaTune enabled, QudaVerbosity verbosity)
  {
#ifdef LAUNCH_TIMER
    launchTimer.TPSTART(QUDA_PROFILE_TOTAL);
    launchTimer.TPSTART(QUDA_PROFILE_INIT);
#endif

    TuneKey key = tunable.tuneKey();
    if (use_managed_memory()) strcat(key.aux, ",managed");
    last_key = key;

#ifdef LAUNCH_TIMER
    launchTimer.TPSTOP(QUDA_PROFILE_INIT);
    launchTimer.TPSTART(QUDA_PROFILE_PREAMBLE);
#endif

    static const Tunable *active_tunable; // for error checking
    it = tunecache.find(key);

    // first check if we have the tuned value and return if we have it
    if (enabled == QUDA_TUNE_YES && it != tunecache.end()) {

#ifdef LAUNCH_TIMER
      launchTimer.TPSTOP(QUDA_PROFILE_PREAMBLE);
      launchTimer.TPSTART(QUDA_PROFILE_COMPUTE);
#endif

      TuneParam &param_tuned = it->second;

      if (verbosity >= QUDA_DEBUG_VERBOSE) {
        printfQuda("Launching %s with %s at vol=%s with %s\n", key.name, key.aux, key.volume,
                   tunable.paramString(param_tuned).c_str());
      }

#ifdef LAUNCH_TIMER
      launchTimer.TPSTOP(QUDA_PROFILE_COMPUTE);
      launchTimer.TPSTART(QUDA_PROFILE_EPILOGUE);
#endif

      tunable.checkLaunchParam(param_tuned);

      // we could be tuning outside of the current scope
      if (!tuning && profile_count) param_tuned.n_calls++;

#ifdef LAUNCH_TIMER
      launchTimer.TPSTOP(QUDA_PROFILE_EPILOGUE);
      launchTimer.TPSTOP(QUDA_PROFILE_TOTAL);
#endif

      if (traceEnabled() >= 2) {
        TraceKey trace_entry(key, param_tuned.time);
        trace_list.push_back(trace_entry);
      }

      return param_tuned;
    }

#ifdef LAUNCH_TIMER
    launchTimer.TPSTOP(QUDA_PROFILE_PREAMBLE);
    launchTimer.TPSTOP(QUDA_PROFILE_TOTAL);
#endif

    static TuneParam param;

    if (enabled == QUDA_TUNE_NO) {
      TuneParam param_default;
      param_default.aux = make_int4(-1, -1, -1, -1);
      tunable.defaultTuneParam(param_default);
      tunable.checkLaunchParam(param_default);
      if (verbosity >= QUDA_DEBUG_VERBOSE) {
        printfQuda("Launching %s with %s at vol=%s with %s (untuned)\n", key.name, key.aux, key.volume,
                   tunable.paramString(param_default).c_str());
      }

      return param_default;
    } else if (!tuning) {

      /* As long as global reductions are not disabled, only do the
         tuning on node 0, else do the tuning on all nodes since we
         can't guarantee that all nodes are partaking */
      if (comm_rank_global() == 0 || !commGlobalReduction() || policyTuning() || uberTuning()) {
        TuneParam best_param;
        TuneCandidates tc(tunable.num_candidates());
        float best_time;
        time_t now;

        tuning = true;
        active_tunable = &tunable;
        best_time = FLT_MAX;

        if (verbosity >= QUDA_DEBUG_VERBOSE) printfQuda("PreTune %s\n", key.name);
        tunable.preTune();

        if (verbosity >= QUDA_DEBUG_VERBOSE) {
          printfQuda("Tuning %s with %s at vol=%s\n", key.name, key.aux, key.volume);
        }

        const auto &stream = device::get_default_stream();
        device_timer_t timer(stream);

        host_timer_t tune_timer;
        tune_timer.start(__func__, __FILE__, __LINE__);

        param.aux = make_int4(-1, -1, -1, -1);
        tunable.initTuneParam(param);

        auto error = QUDA_SUCCESS;
        const int candidate_iterations = tunable.candidate_iter();
        while (tuning && candidatetuning) {
          qudaDeviceSynchronize();
          tunable.checkLaunchParam(param);
          if (verbosity >= QUDA_DEBUG_VERBOSE) {
            printfQuda("About to call tunable.apply block=(%d,%d,%d) grid=(%d,%d,%d) shared_bytes=%d aux=(%d,%d,%d,%d)\n",
                       static_cast<int>(param.block.x), static_cast<int>(param.block.y),
                       static_cast<int>(param.block.z), static_cast<int>(param.grid.x), static_cast<int>(param.grid.y),
                       static_cast<int>(param.grid.z), static_cast<int>(param.shared_bytes),
                       static_cast<int>(param.aux.x), static_cast<int>(param.aux.y), static_cast<int>(param.aux.z), static_cast<int>(param.aux.w));
          }

          tunable.apply(stream); // do initial call in case we need to jit compile for these parameters or if policy tuning

          timer.start();
          for (int i = 0; i < candidate_iterations; i++) {
            tunable.apply(stream); // calls tuneLaunch() again, which simply returns the currently active param
          }
          timer.stop();
          qudaDeviceSynchronize();
          error = qudaGetLastError();

          if (error != QUDA_SUCCESS) { // check we don't have a sticky error
            qudaDeviceSynchronize();
            if (qudaGetLastError() != QUDA_SUCCESS)
              errorQuda("Failed to clear error state %s\n", qudaGetLastErrorString().c_str());
          }

          float elapsed_time = timer.last() / candidate_iterations;
          param.time = elapsed_time;
          if ((error == QUDA_SUCCESS) && (tunable.launchError() == QUDA_SUCCESS)) tc.pushCandidate(param);

          if ((verbosity >= QUDA_DEBUG_VERBOSE)) {
            if (error == QUDA_SUCCESS && tunable.launchError() == QUDA_SUCCESS) {
              printfQuda("C   %s gives %s\n", tunable.paramString(param).c_str(),
                         tunable.perfString(elapsed_time).c_str());
            } else {
              printfQuda("    %s gives %s\n", tunable.paramString(param).c_str(), qudaGetLastErrorString().c_str());
              error = QUDA_SUCCESS;
            }
          }
          candidatetuning = tunable.advanceTuneParam(param);
          tunable.launchError() = QUDA_SUCCESS;
        }

        if (tc.empty()) {
          if (error != QUDA_SUCCESS) warningQuda("Last error: %s\n", qudaGetLastErrorString().c_str());
          errorQuda("Auto-tuning failed for %s with %s at vol=%s", key.name, key.aux, key.volume);
        }

        const float min_tune_time = tunable.min_tune_time();
        const int min_tune_iterations = tunable.min_tune_iter();

        if (policyTuning() || uberTuning()) { tc.broadcast(); }
        const int tuneiterations
          = std::max(static_cast<int>(std::ceil(min_tune_time / tc.getBestTime())), min_tune_iterations);
        if ((verbosity >= QUDA_DEBUG_VERBOSE)) {
          printfQuda("Candidate tuning finished for %s with %s. Best time %f and now continuing with %i iterations.\n",
                     key.name, key.aux, tc.getBestTime(), tuneiterations);
        }

        // we now have the candidates, now need to loop over candidates
        while (!tc.empty()) {
          param = tc.top();
          qudaDeviceSynchronize();
          tunable.checkLaunchParam(param);
          if (verbosity >= QUDA_DEBUG_VERBOSE) {
            printfQuda("About to call tunable.apply block=(%d,%d,%d) grid=(%d,%d,%d) shared_bytes=%d aux=(%d,%d,%d,%d)\n",
                       static_cast<int>(param.block.x), static_cast<int>(param.block.y),
                       static_cast<int>(param.block.z), static_cast<int>(param.grid.x), static_cast<int>(param.grid.y),
                       static_cast<int>(param.grid.z), static_cast<int>(param.shared_bytes),
                       static_cast<int>(param.aux.x), static_cast<int>(param.aux.y), static_cast<int>(param.aux.z), static_cast<int>(param.aux.w));
          }

          tunable.apply(stream); // do warm up call, for consistency with the candidate tuning
          timer.start();
          for (int i = 0; i < tuneiterations; i++) {
            tunable.apply(stream); // calls tuneLaunch() again, which simply returns the currently active param
          }
          timer.stop();
          qudaDeviceSynchronize();
          auto error = qudaGetLastError();

          if (error != QUDA_SUCCESS) { // check we don't have a sticky error
            qudaDeviceSynchronize();
            if (qudaGetLastError() != QUDA_SUCCESS)
              errorQuda("Failed to clear error state %s\n", qudaGetLastErrorString().c_str());
          }

          float elapsed_time = timer.last() / tuneiterations;

          if ((elapsed_time < best_time) && (error == QUDA_SUCCESS) && (tunable.launchError() == QUDA_SUCCESS)) {
            best_time = elapsed_time;
            best_param = param;
          }
          if ((verbosity >= QUDA_DEBUG_VERBOSE)) {
            if (error == QUDA_SUCCESS && tunable.launchError() == QUDA_SUCCESS) {
              printfQuda("T   %s gives %s\n", tunable.paramString(param).c_str(),
                         tunable.perfString(elapsed_time).c_str());
            } else {
              printfQuda("    %s gives %s\n", tunable.paramString(param).c_str(), qudaGetLastErrorString().c_str());
            }
          }

          tunable.launchError() = QUDA_SUCCESS;
          tc.pop();
        }

        tuning = false;
        candidatetuning = true;
        tune_timer.stop(__func__, __FILE__, __LINE__);

        if (verbosity >= QUDA_VERBOSE) {
          printfQuda("Tuned %s giving %s for %s with %s\n", tunable.paramString(best_param).c_str(),
                     tunable.perfString(best_time).c_str(), key.name, key.aux);
        }

        auto regression_tol = 1.1;
        if (best_time > regression_tol * tc.getBestTime() && best_time > 1e-5) {
          warningQuda("Unexpected regression when tuning candidates for %s: (%g > %g * %g)",
                      key.name, best_time, regression_tol, tc.getBestTime());
        }

        time(&now);
        best_param.comment = "# " + tunable.perfString(best_time) + tunable.miscString(best_param);
        best_param.comment += ", tuning took " + std::to_string(tune_timer.last()) + " seconds at ";
        best_param.comment += ctime(&now); // includes a newline
        best_param.time = best_time;

        if (verbosity >= QUDA_DEBUG_VERBOSE) printfQuda("PostTune %s\n", key.name);
        tuning = true;
        tunable.postTune();
        tuning = false;
        param = best_param;
        tunecache[key] = best_param;
      }
      if (commGlobalReduction() || policyTuning() || uberTuning()) { broadcastTuneCache(); }

      {
        static host_timer_t time_since_save;
        if (!time_since_save.running) time_since_save.start();

        const double max_time = 120; // dump the tunecache every 120 seconds
        time_since_save.peek();
        if (time_since_save.last() > max_time) {
          time_since_save.stop();
          saveTuneCache();
          time_since_save.start();
        }
      }

      // check this process is getting the key that is expected
      if (tunecache.find(key) == tunecache.end()) {

        // if we can't find the key, and debugging, then print out the entire map
        if (verbosity >= QUDA_DEBUG_VERBOSE)
          for (auto elem : tunecache) std::cout << elem.first << ": " << elem.second << std::endl;

        errorQuda("Failed to find key entry (%s:%s:%s)", key.name, key.volume, key.aux);
      }
      param = tunecache[key]; // read this now for all processes

      if (traceEnabled() >= 2) {
        TraceKey trace_entry(key, param.time);
        trace_list.push_back(trace_entry);
      }

    } else if (&tunable != active_tunable) {
      errorQuda("Unexpected call to tuneLaunch() in %s::apply()", typeid(tunable).name());
    }

    param.n_calls = profile_count ? 1 : 0;

    return param;
  }

  void printLaunchTimer()
  {
#ifdef LAUNCH_TIMER
    launchTimer.Print();
#endif
  }
} // namespace quda
