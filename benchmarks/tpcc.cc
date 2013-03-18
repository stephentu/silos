#include <sys/time.h>
#include <string>
#include <ctype.h>
#include <stdlib.h>

#include <stdlib.h>
#include <unistd.h>
#include <sched.h>

#include <set>
#include <vector>

#include "bench.h"
#include "tpcc.h"
#include "../txn.h"
#include "../macros.h"
#include "../scopedperf.hh"

// tpcc schemas
using namespace std;
using namespace util;

typedef uint uint;

// config constants

static inline ALWAYS_INLINE size_t
NumWarehouses()
{
  return (size_t) scale_factor;
}

static constexpr inline ALWAYS_INLINE size_t
NumItems()
{
  return 100000;
}

static constexpr inline ALWAYS_INLINE size_t
NumDistrictsPerWarehouse()
{
  return 10;
}

static constexpr inline ALWAYS_INLINE size_t
NumCustomersPerDistrict()
{
  return 3000;
}

struct checker {
  // these sanity checks are just a few simple checks to make sure
  // the data is not entirely corrupted

  static inline ALWAYS_INLINE void
  SanityCheckCustomer(const customer::key *k, const customer::value *v)
  {
    INVARIANT(k->c_w_id >= 1 && static_cast<size_t>(k->c_w_id) <= NumWarehouses());
    INVARIANT(k->c_d_id >= 1 && static_cast<size_t>(k->c_d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(k->c_id >= 1 && static_cast<size_t>(k->c_id) <= NumCustomersPerDistrict());
    INVARIANT(v->c_credit == "BC" || v->c_credit == "GC");
    INVARIANT(v->c_middle == "OE");
  }

  static inline ALWAYS_INLINE void
  SanityCheckWarehouse(const warehouse::key *k, const warehouse::value *v)
  {
    INVARIANT(k->w_id >= 1 && static_cast<size_t>(k->w_id) <= NumWarehouses());
    INVARIANT(v->w_state.size() == 2);
    INVARIANT(v->w_zip == "123456789");
  }

  static inline ALWAYS_INLINE void
  SanityCheckDistrict(const district::key *k, const district::value *v)
  {
    INVARIANT(k->d_w_id >= 1 && static_cast<size_t>(k->d_w_id) <= NumWarehouses());
    INVARIANT(k->d_id >= 1 && static_cast<size_t>(k->d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(v->d_next_o_id >= 3001);
    INVARIANT(v->d_state.size() == 2);
    INVARIANT(v->d_zip == "123456789");
  }

  static inline ALWAYS_INLINE void
  SanityCheckItem(const item::key *k, const item::value *v)
  {
    INVARIANT(k->i_id >= 1 && static_cast<size_t>(k->i_id) <= NumItems());
    INVARIANT(v->i_price >= 1.0 && v->i_price <= 100.0);
  }

  static inline ALWAYS_INLINE void
  SanityCheckStock(const stock::key *k, const stock::value *v)
  {
    INVARIANT(k->s_w_id >= 1 && static_cast<size_t>(k->s_w_id) <= NumWarehouses());
    INVARIANT(k->s_i_id >= 1 && static_cast<size_t>(k->s_i_id) <= NumItems());
  }

  static inline ALWAYS_INLINE void
  SanityCheckNewOrder(const new_order::key *k, const new_order::value *v)
  {
    INVARIANT(k->no_w_id >= 1 && static_cast<size_t>(k->no_w_id) <= NumWarehouses());
    INVARIANT(k->no_d_id >= 1 && static_cast<size_t>(k->no_d_id) <= NumDistrictsPerWarehouse());
  }

  static inline ALWAYS_INLINE void
  SanityCheckOOrder(const oorder::key *k, const oorder::value *v)
  {
    INVARIANT(k->o_w_id >= 1 && static_cast<size_t>(k->o_w_id) <= NumWarehouses());
    INVARIANT(k->o_d_id >= 1 && static_cast<size_t>(k->o_d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(v->o_c_id >= 1 && static_cast<size_t>(v->o_c_id) <= NumCustomersPerDistrict());
    INVARIANT(v->o_carrier_id >= 0 && static_cast<size_t>(v->o_carrier_id) <= NumDistrictsPerWarehouse());
    INVARIANT(v->o_ol_cnt >= 5 && v->o_ol_cnt <= 15);
  }

  static inline ALWAYS_INLINE void
  SanityCheckOrderLine(const order_line::key *k, const order_line::value *v)
  {
    INVARIANT(k->ol_w_id >= 1 && static_cast<size_t>(k->ol_w_id) <= NumWarehouses());
    INVARIANT(k->ol_d_id >= 1 && static_cast<size_t>(k->ol_d_id) <= NumDistrictsPerWarehouse());
    INVARIANT(k->ol_number >= 1 && k->ol_number <= 15);
    INVARIANT(v->ol_i_id >= 1 && static_cast<size_t>(v->ol_i_id) <= NumItems());
  }

};

class tpcc_worker_mixin {
public:
  tpcc_worker_mixin(const map<string, abstract_ordered_index *> &open_tables) :
      tbl_customer(open_tables.at("customer")),
      tbl_customer_name_idx(open_tables.at("customer_name_idx")),
      tbl_district(open_tables.at("district")),
      tbl_history(open_tables.at("history")),
      tbl_item(open_tables.at("item")),
      tbl_new_order(open_tables.at("new_order")),
      tbl_oorder(open_tables.at("oorder")),
      tbl_oorder_c_id_idx(open_tables.at("oorder_c_id_idx")),
      tbl_order_line(open_tables.at("order_line")),
      tbl_stock(open_tables.at("stock")),
      tbl_warehouse(open_tables.at("warehouse"))
  {
    assert(NumWarehouses() >= 1);
  }

protected:

  abstract_ordered_index *tbl_customer;
  abstract_ordered_index *tbl_customer_name_idx;
  abstract_ordered_index *tbl_district;
  abstract_ordered_index *tbl_history;
  abstract_ordered_index *tbl_item;
  abstract_ordered_index *tbl_new_order;
  abstract_ordered_index *tbl_oorder;
  abstract_ordered_index *tbl_oorder_c_id_idx;
  abstract_ordered_index *tbl_order_line;
  abstract_ordered_index *tbl_stock;
  abstract_ordered_index *tbl_warehouse;

  // only TPCC loaders need to call this- workers are automatically
  // pinned by their worker id (which corresponds to warehouse id
  // in TPCC)
  //
  // pins the *calling* thread
  static void
  PinToWarehouseId(unsigned int wid)
  {
    ALWAYS_ASSERT(CPU_SETSIZE >= coreid::num_cpus_online());
    ALWAYS_ASSERT(wid >= 1 && wid <= NumWarehouses());
    const unsigned long pinid = (wid - 1) % coreid::num_cpus_online();
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(pinid, &cs);
    ALWAYS_ASSERT(sched_setaffinity(0, sizeof(cs), &cs) == 0);
    INVARIANT(IsPinnedToWarehouseId(wid));
  }

  // checks the *calling* thread
  static bool
  IsPinnedToWarehouseId(unsigned int wid)
  {
    ALWAYS_ASSERT(CPU_SETSIZE >= coreid::num_cpus_online());
    ALWAYS_ASSERT(wid >= 1 && wid <= NumWarehouses());
    const unsigned long pinid = (wid - 1) % coreid::num_cpus_online();
    cpu_set_t cs;
    CPU_ZERO(&cs);
    ALWAYS_ASSERT(sched_getaffinity(0, sizeof(cs), &cs) == 0);
    return CPU_ISSET(pinid, &cs);
  }

public:

  static inline uint32_t
  GetCurrentTimeMillis()
  {
    //struct timeval tv;
    //ALWAYS_ASSERT(gettimeofday(&tv, 0) == 0);
    //return tv.tv_sec * 1000;

    // XXX(stephentu): implement a scalable GetCurrentTimeMillis()
    // for now, we just give each core an increasing number

    static __thread uint32_t tl_hack = 0;
    return tl_hack++;
  }

  // utils for generating random #s and strings

  static inline ALWAYS_INLINE int
  CheckBetweenInclusive(int v, int lower, int upper)
  {
    INVARIANT(v >= lower);
    INVARIANT(v <= upper);
    return v;
  }

  static inline ALWAYS_INLINE int
  RandomNumber(fast_random &r, int min, int max)
  {
    return CheckBetweenInclusive((int) (r.next_uniform() * (max - min + 1) + min), min, max);
  }

  static inline ALWAYS_INLINE int
  NonUniformRandom(fast_random &r, int A, int C, int min, int max)
  {
    return (((RandomNumber(r, 0, A) | RandomNumber(r, min, max)) + C) % (max - min + 1)) + min;
  }

  static inline ALWAYS_INLINE int
  GetItemId(fast_random &r)
  {
    return CheckBetweenInclusive(NonUniformRandom(r, 8191, 7911, 1, NumItems()), 1, NumItems());
  }

  static inline ALWAYS_INLINE int
  GetCustomerId(fast_random &r)
  {
    return CheckBetweenInclusive(NonUniformRandom(r, 1023, 259, 1, NumCustomersPerDistrict()), 1, NumCustomersPerDistrict());
  }

  static string NameTokens[];

  // all tokens are at most 5 chars long
  static const size_t CustomerLastNameMaxSize = 5 * 3;

  static inline size_t
  GetCustomerLastName(uint8_t *buf, fast_random &r, int num)
  {
    const string &s0 = NameTokens[num / 100];
    const string &s1 = NameTokens[(num / 10) % 10];
    const string &s2 = NameTokens[num % 10];
    uint8_t *const begin = buf;
    const size_t s0_sz = s0.size();
    const size_t s1_sz = s1.size();
    const size_t s2_sz = s2.size();
    NDB_MEMCPY(buf, s0.data(), s0_sz); buf += s0_sz;
    NDB_MEMCPY(buf, s1.data(), s1_sz); buf += s1_sz;
    NDB_MEMCPY(buf, s2.data(), s2_sz); buf += s2_sz;
    return buf - begin;
  }

  static inline ALWAYS_INLINE size_t
  GetCustomerLastName(char *buf, fast_random &r, int num)
  {
    return GetCustomerLastName((uint8_t *) buf, r, num);
  }

  static inline string
  GetCustomerLastName(fast_random &r, int num)
  {
    string ret;
    ret.resize(CustomerLastNameMaxSize);
    ret.resize(GetCustomerLastName((uint8_t *) &ret[0], r, num));
    return ret;
  }

  static inline ALWAYS_INLINE string
  GetNonUniformCustomerLastNameLoad(fast_random &r)
  {
    return GetCustomerLastName(r, NonUniformRandom(r, 255, 157, 0, 999));
  }

  static inline ALWAYS_INLINE size_t
  GetNonUniformCustomerLastNameRun(uint8_t *buf, fast_random &r)
  {
    return GetCustomerLastName(buf, r, NonUniformRandom(r, 255, 223, 0, 999));
  }

  static inline ALWAYS_INLINE size_t
  GetNonUniformCustomerLastNameRun(char *buf, fast_random &r)
  {
    return GetNonUniformCustomerLastNameRun((uint8_t *) buf, r);
  }

  static inline ALWAYS_INLINE string
  GetNonUniformCustomerLastNameRun(fast_random &r)
  {
    return GetCustomerLastName(r, NonUniformRandom(r, 255, 223, 0, 999));
  }

  // following oltpbench, we really generate strings of len - 1...
  static inline string
  RandomStr(fast_random &r, uint len)
  {
    // this is a property of the oltpbench implementation...
    if (!len)
      return "";

    uint i = 0;
    string buf(len - 1, 0);
    while (i < (len - 1)) {
      const char c = (char) r.next_char();
      // XXX(stephentu): oltpbench uses java's Character.isLetter(), which
      // is a less restrictive filter than isalnum()
      if (!isalnum(c))
        continue;
      buf[i++] = c;
    }
    return buf;
  }

  // RandomNStr() actually produces a string of length len
  static inline string
  RandomNStr(fast_random &r, uint len)
  {
    const char base = '0';
    string buf(len, 0);
    for (uint i = 0; i < len; i++)
      buf[i] = (char)(base + (r.next() % 10));
    return buf;
  }
};

string tpcc_worker_mixin::NameTokens[] =
  {
    string("BAR"),
    string("OUGHT"),
    string("ABLE"),
    string("PRI"),
    string("PRES"),
    string("ESE"),
    string("ANTI"),
    string("CALLY"),
    string("ATION"),
    string("EING"),
  };

STATIC_COUNTER_DECL(scopedperf::tod_ctr, tpcc_txn_tod, tpcc_txn_cg)

class tpcc_worker : public bench_worker, public tpcc_worker_mixin {
public:
  tpcc_worker(unsigned int worker_id,
              unsigned long seed, abstract_db *db,
              const map<string, abstract_ordered_index *> &open_tables,
              spin_barrier *barrier_a, spin_barrier *barrier_b,
              uint warehouse_id)
    : bench_worker(worker_id, seed, db, open_tables, barrier_a, barrier_b),
      tpcc_worker_mixin(open_tables),
      warehouse_id(warehouse_id)
  {
    INVARIANT(warehouse_id >= 1);
    INVARIANT(warehouse_id <= NumWarehouses());
    NDB_MEMSET(&last_no_o_ids[0], 0, sizeof(last_no_o_ids));
  }

  // XXX(stephentu): tune this
  static const size_t NMaxCustomerIdxScanElems = 512;

  ssize_t txn_new_order();

  static ssize_t
  TxnNewOrder(bench_worker *w)
  {
    ANON_REGION("TxnNewOrder:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_new_order();
  }

  ssize_t txn_delivery();

  static ssize_t
  TxnDelivery(bench_worker *w)
  {
    ANON_REGION("TxnDelivery:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_delivery();
  }

  ssize_t txn_payment();

  static ssize_t
  TxnPayment(bench_worker *w)
  {
    ANON_REGION("TxnPayment:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_payment();
  }

  ssize_t txn_order_status();

  static ssize_t
  TxnOrderStatus(bench_worker *w)
  {
    ANON_REGION("TxnOrderStatus:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_order_status();
  }

  ssize_t txn_stock_level();

  static ssize_t
  TxnStockLevel(bench_worker *w)
  {
    ANON_REGION("TxnStockLevel:", &tpcc_txn_cg);
    return static_cast<tpcc_worker *>(w)->txn_stock_level();
  }

  virtual workload_desc_vec
  get_workload() const
  {
    workload_desc_vec w;
    // numbers from sigmod.csail.mit.edu:
    //w.push_back(workload_desc("NewOrder", 1.0, TxnNewOrder)); // ~10k ops/sec
    //w.push_back(workload_desc("Payment", 1.0, TxnPayment)); // ~32k ops/sec
    //w.push_back(workload_desc("Delivery", 1.0, TxnDelivery)); // ~104k ops/sec
    //w.push_back(workload_desc("OrderStatus", 1.0, TxnOrderStatus)); // ~33k ops/sec
    //w.push_back(workload_desc("StockLevel", 1.0, TxnStockLevel)); // ~2k ops/sec

    w.push_back(workload_desc("NewOrder", 0.45, TxnNewOrder));
    w.push_back(workload_desc("Payment", 0.43, TxnPayment));
    w.push_back(workload_desc("Delivery", 0.04, TxnDelivery));
    w.push_back(workload_desc("OrderStatus", 0.04, TxnOrderStatus));
    w.push_back(workload_desc("StockLevel", 0.04, TxnStockLevel));
    return w;
  }

protected:

  virtual void
  on_run_setup() OVERRIDE
  {
    if (pin_cpus)
      ALWAYS_ASSERT(IsPinnedToWarehouseId(warehouse_id));
  }

  inline ALWAYS_INLINE string &
  str() {
    // XXX: hacky for now
    string *px = arena.next();
    ALWAYS_ASSERT(px);
    return *px;
  }

private:
  const uint warehouse_id;
  int32_t last_no_o_ids[10]; // XXX(stephentu): hack

  // some scratch buffer space
  string obj_key0;
  string obj_key1;
  string obj_v;
};

class tpcc_warehouse_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_warehouse_loader(unsigned long seed,
                        abstract_db *db,
                        const map<string, abstract_ordered_index *> &open_tables)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(open_tables)
  {}

protected:
  virtual void
  load()
  {
    string obj_buf;
    void *txn = db->new_txn(txn_flags, txn_buf());
    uint64_t warehouse_total_sz = 0, n_warehouses = 0;
    try {
      vector<warehouse::value> warehouses;
      for (uint i = 1; i <= NumWarehouses(); i++) {
        // seems kind of silly to change affinity to insert 1 data item, but
        // whatever we'll live

        if (pin_cpus)
          PinToWarehouseId(i);

        const warehouse::key k(i);

        const string w_name = RandomStr(r, RandomNumber(r, 6, 10));
        const string w_street_1 = RandomStr(r, RandomNumber(r, 10, 20));
        const string w_street_2 = RandomStr(r, RandomNumber(r, 10, 20));
        const string w_city = RandomStr(r, RandomNumber(r, 10, 20));
        const string w_state = RandomStr(r, 3);
        const string w_zip = "123456789";

        warehouse::value v;
        v.w_ytd = 300000;
        v.w_tax = (float) RandomNumber(r, 0, 2000) / 10000.0;
        v.w_name.assign(w_name);
        v.w_street_1.assign(w_street_1);
        v.w_street_2.assign(w_street_2);
        v.w_city.assign(w_city);
        v.w_state.assign(w_state);
        v.w_zip.assign(w_zip);

        checker::SanityCheckWarehouse(&k, &v);
        const size_t sz = Size(v);
        warehouse_total_sz += sz;
        n_warehouses++;
        tbl_warehouse->insert(txn, Encode(k), Encode(obj_buf, v));

        warehouses.push_back(v);
      }
      ALWAYS_ASSERT(db->commit_txn(txn));
      txn = db->new_txn(txn_flags, txn_buf());
      for (uint i = 1; i <= NumWarehouses(); i++) {
        const warehouse::key k(i);
        string warehouse_v;
        ALWAYS_ASSERT(tbl_warehouse->get(txn, Encode(k), warehouse_v));
        warehouse::value warehouse_temp;
        const warehouse::value *v = Decode(warehouse_v, warehouse_temp);
        ALWAYS_ASSERT(warehouses[i - 1] == *v);

        checker::SanityCheckWarehouse(&k, v);
      }
      ALWAYS_ASSERT(db->commit_txn(txn));
    } catch (abstract_db::abstract_abort_exception &ex) {
      // shouldn't abort on loading!
      ALWAYS_ASSERT(false);
    }
    if (verbose) {
      cerr << "[INFO] finished loading warehouse" << endl;
      cerr << "[INFO]   * average warehouse record length: "
           << (double(warehouse_total_sz)/double(n_warehouses)) << " bytes" << endl;
    }
  }
};

class tpcc_item_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_item_loader(unsigned long seed,
                   abstract_db *db,
                   const map<string, abstract_ordered_index *> &open_tables)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(open_tables)
  {}

protected:
  virtual void
  load()
  {
    string obj_buf;
    const ssize_t bsize = db->txn_max_batch_size();
    void *txn = db->new_txn(txn_flags, txn_buf());
    uint64_t total_sz = 0;
    try {
      for (uint i = 1; i <= NumItems(); i++) {
        // items don't "belong" to a certain warehouse, so no pinning
        const item::key k(i);

        item::value v;
        const string i_name = RandomStr(r, RandomNumber(r, 14, 24));
        v.i_name.assign(i_name);
        v.i_price = (float) RandomNumber(r, 100, 10000) / 100.0;
        const int len = RandomNumber(r, 26, 50);
        if (RandomNumber(r, 1, 100) > 10) {
          const string i_data = RandomStr(r, len);
          v.i_data.assign(i_data);
        } else {
          const int startOriginal = RandomNumber(r, 2, (len - 8));
          const string i_data = RandomStr(r, startOriginal + 1) + "ORIGINAL" + RandomStr(r, len - startOriginal - 7);
          v.i_data.assign(i_data);
        }
        v.i_im_id = RandomNumber(r, 1, 10000);

        checker::SanityCheckItem(&k, &v);
        const size_t sz = Size(v);
        total_sz += sz;
        tbl_item->insert(txn, Encode(k), Encode(obj_buf, v));

        if (bsize != -1 && !(i % bsize)) {
          ALWAYS_ASSERT(db->commit_txn(txn));
          txn = db->new_txn(txn_flags, txn_buf());
        }
      }
      ALWAYS_ASSERT(db->commit_txn(txn));
    } catch (abstract_db::abstract_abort_exception &ex) {
      // shouldn't abort on loading!
      ALWAYS_ASSERT(false);
    }
    if (verbose) {
      cerr << "[INFO] finished loading item" << endl;
      cerr << "[INFO]   * average item record length: "
           << (double(total_sz)/double(NumItems())) << " bytes" << endl;
    }
  }
};

class tpcc_stock_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_stock_loader(unsigned long seed,
                    abstract_db *db,
                    const map<string, abstract_ordered_index *> &open_tables,
                    ssize_t warehouse_id)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(open_tables),
      warehouse_id(warehouse_id)
  {
    ALWAYS_ASSERT(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

protected:
  virtual void
  load()
  {
    string obj_buf;

    uint64_t stock_total_sz = 0, n_stocks = 0;
    const uint w_start = (warehouse_id == -1) ?
      1 : static_cast<uint>(warehouse_id);
    const uint w_end   = (warehouse_id == -1) ?
      NumWarehouses() : static_cast<uint>(warehouse_id);

    for (uint w = w_start; w <= w_end; w++) {
      const size_t NBatches = 1000;
      const size_t NItemsPerBatch = NumItems();

      static_assert(NumItems() % NBatches == 0, "xx");
      static_assert(NumItems() >= NBatches, "xx");

      if (pin_cpus)
        PinToWarehouseId(w);

      for (uint b = 0; b < NItemsPerBatch;) {
        void * const txn = db->new_txn(txn_flags, txn_buf());
        try {
          for (uint i = (b * NItemsPerBatch + 1); i <= NItemsPerBatch; i++) {
            const stock::key k(w, i);

            stock::value v;
            v.s_quantity = RandomNumber(r, 10, 100);
            v.s_ytd = 0;
            v.s_order_cnt = 0;
            v.s_remote_cnt = 0;
            const int len = RandomNumber(r, 26, 50);
            if (RandomNumber(r, 1, 100) > 10) {
              const string s_data = RandomStr(r, len);
              v.s_data.assign(s_data);
            } else {
              const int startOriginal = RandomNumber(r, 2, (len - 8));
              const string s_data = RandomStr(r, startOriginal + 1) + "ORIGINAL" + RandomStr(r, len - startOriginal - 7);
              v.s_data.assign(s_data);
            }
            v.s_dist_01.assign(RandomStr(r, 24));
            v.s_dist_02.assign(RandomStr(r, 24));
            v.s_dist_03.assign(RandomStr(r, 24));
            v.s_dist_04.assign(RandomStr(r, 24));
            v.s_dist_05.assign(RandomStr(r, 24));
            v.s_dist_06.assign(RandomStr(r, 24));
            v.s_dist_07.assign(RandomStr(r, 24));
            v.s_dist_08.assign(RandomStr(r, 24));
            v.s_dist_09.assign(RandomStr(r, 24));
            v.s_dist_10.assign(RandomStr(r, 24));

            checker::SanityCheckStock(&k, &v);
            const size_t sz = Size(v);
            stock_total_sz += sz;
            n_stocks++;
            tbl_stock->insert(txn, Encode(k), Encode(obj_buf, v));
          }
          if (db->commit_txn(txn)) {
            b++;
          } else {
            db->abort_txn(txn);
            if (verbose)
              cerr << "[WARNING] stock loader loading abort" << endl;
          }
        } catch (abstract_db::abstract_abort_exception &ex) {
          db->abort_txn(txn);
          ALWAYS_ASSERT(warehouse_id != -1);
          if (verbose)
            cerr << "[WARNING] stock loader loading abort" << endl;
        }
      }
    }

    if (verbose) {
      if (warehouse_id == -1) {
        cerr << "[INFO] finished loading stock" << endl;
        cerr << "[INFO]   * average stock record length: "
             << (double(stock_total_sz)/double(n_stocks)) << " bytes" << endl;
      } else {
        cerr << "[INFO] finished loading stock (w=" << warehouse_id << ")" << endl;
      }
    }
  }

private:
  ssize_t warehouse_id;
};

class tpcc_district_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_district_loader(unsigned long seed,
                       abstract_db *db,
                       const map<string, abstract_ordered_index *> &open_tables)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(open_tables)
  {}

protected:
  virtual void
  load()
  {
    string obj_buf;

    const ssize_t bsize = db->txn_max_batch_size();
    void *txn = db->new_txn(txn_flags, txn_buf());
    uint64_t district_total_sz = 0, n_districts = 0;
    try {
      uint cnt = 0;
      for (uint w = 1; w <= NumWarehouses(); w++) {
        if (pin_cpus)
          PinToWarehouseId(w);
        for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++, cnt++) {
          const district::key k(w, d);

          district::value v;
          v.d_ytd = 30000;
          v.d_tax = (float) (RandomNumber(r, 0, 2000) / 10000.0);
          v.d_next_o_id = 3001;
          v.d_name.assign(RandomStr(r, RandomNumber(r, 6, 10)));
          v.d_street_1.assign(RandomStr(r, RandomNumber(r, 10, 20)));
          v.d_street_2.assign(RandomStr(r, RandomNumber(r, 10, 20)));
          v.d_city.assign(RandomStr(r, RandomNumber(r, 10, 20)));
          v.d_state.assign(RandomStr(r, 3));
          v.d_zip.assign("123456789");

          checker::SanityCheckDistrict(&k, &v);
          const size_t sz = Size(v);
          district_total_sz += sz;
          n_districts++;
          tbl_district->insert(txn, Encode(k), Encode(obj_buf, v));

          if (bsize != -1 && !((cnt + 1) % bsize)) {
            ALWAYS_ASSERT(db->commit_txn(txn));
            txn = db->new_txn(txn_flags, txn_buf());
          }
        }
      }
      ALWAYS_ASSERT(db->commit_txn(txn));
    } catch (abstract_db::abstract_abort_exception &ex) {
      // shouldn't abort on loading!
      ALWAYS_ASSERT(false);
    }
    if (verbose) {
      cerr << "[INFO] finished loading district" << endl;
      cerr << "[INFO]   * average district record length: "
           << (double(district_total_sz)/double(n_districts)) << " bytes" << endl;
    }
  }
};

class tpcc_customer_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_customer_loader(unsigned long seed,
                       abstract_db *db,
                       const map<string, abstract_ordered_index *> &open_tables,
                       ssize_t warehouse_id)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(open_tables),
      warehouse_id(warehouse_id)
  {
    ALWAYS_ASSERT(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

protected:
  virtual void
  load()
  {
    string obj_buf;

    const uint w_start = (warehouse_id == -1) ?
      1 : static_cast<uint>(warehouse_id);
    const uint w_end   = (warehouse_id == -1) ?
      NumWarehouses() : static_cast<uint>(warehouse_id);

    uint64_t total_sz = 0;

    for (uint w = w_start; w <= w_end; w++) {
      if (pin_cpus)
        PinToWarehouseId(w);
      for (uint d = 1; d <= NumDistrictsPerWarehouse();) {
        void * const txn = db->new_txn(txn_flags, txn_buf());
        try {
          for (uint c = 1; c <= NumCustomersPerDistrict(); c++) {
            const customer::key k(w, d, c);

            customer::value v;
            v.c_discount = (float) (RandomNumber(r, 1, 5000) / 10000.0);
            if (RandomNumber(r, 1, 100) <= 10)
              v.c_credit.assign("BC");
            else
              v.c_credit.assign("GC");

            if (c <= 1000)
              v.c_last.assign(GetCustomerLastName(r, c - 1));
            else
              v.c_last.assign(GetNonUniformCustomerLastNameLoad(r));

            v.c_first.assign(RandomStr(r, RandomNumber(r, 8, 16)));
            v.c_credit_lim = 50000;

            v.c_balance = -10;
            v.c_ytd_payment = 10;
            v.c_payment_cnt = 1;
            v.c_delivery_cnt = 0;

            v.c_street_1.assign(RandomStr(r, RandomNumber(r, 10, 20)));
            v.c_street_2.assign(RandomStr(r, RandomNumber(r, 10, 20)));
            v.c_city.assign(RandomStr(r, RandomNumber(r, 10, 20)));
            v.c_state.assign(RandomStr(r, 3));
            v.c_zip.assign(RandomNStr(r, 4) + "11111");
            v.c_phone.assign(RandomNStr(r, 16));
            v.c_since = GetCurrentTimeMillis();
            v.c_middle.assign("OE");
            v.c_data.assign(RandomStr(r, RandomNumber(r, 300, 500)));

            checker::SanityCheckCustomer(&k, &v);
            const size_t sz = Size(v);
            total_sz += sz;
            tbl_customer->insert(txn, Encode(k), Encode(obj_buf, v));

            // customer name index
            const customer_name_idx::key k_idx(k.c_w_id, k.c_d_id, v.c_last.str(true), v.c_first.str(true));
            const customer_name_idx::value v_idx(k.c_id);

            // index structure is:
            // (c_w_id, c_d_id, c_last, c_first) -> (c_id)

            tbl_customer_name_idx->insert(txn, Encode(k_idx), Encode(obj_buf, v_idx));

            history::key k_hist;
            k_hist.h_c_id = c;
            k_hist.h_c_d_id = d;
            k_hist.h_c_w_id = w;
            k_hist.h_d_id = d;
            k_hist.h_w_id = w;
            k_hist.h_date = GetCurrentTimeMillis();

            history::value v_hist;
            v_hist.h_amount = 10;
            v_hist.h_data.assign(RandomStr(r, RandomNumber(r, 10, 24)));

            tbl_history->insert(txn, Encode(k_hist), Encode(obj_buf, v_hist));
          }

          if (db->commit_txn(txn)) {
            d++;
          } else {
            db->abort_txn(txn);
            ALWAYS_ASSERT(warehouse_id == -1);
            if (verbose)
              cerr << "[WARNING] customer loader loading abort" << endl;
          }
        } catch (abstract_db::abstract_abort_exception &ex) {
          db->abort_txn(txn);
          ALWAYS_ASSERT(warehouse_id == -1);
          if (verbose)
            cerr << "[WARNING] customer loader loading abort" << endl;
        }
      }
    }

    if (verbose) {
      if (warehouse_id == -1) {
        cerr << "[INFO] finished loading customer" << endl;
        cerr << "[INFO]   * average customer record length: "
             << (double(total_sz)/double(NumWarehouses()*NumDistrictsPerWarehouse()*NumCustomersPerDistrict()))
             << " bytes " << endl;
      } else {
        cerr << "[INFO] finished loading customer (w=" << warehouse_id << ")" << endl;
      }
    }
  }

private:
  ssize_t warehouse_id;
};

class tpcc_order_loader : public bench_loader, public tpcc_worker_mixin {
public:
  tpcc_order_loader(unsigned long seed,
                    abstract_db *db,
                    const map<string, abstract_ordered_index *> &open_tables,
                    ssize_t warehouse_id)
    : bench_loader(seed, db, open_tables),
      tpcc_worker_mixin(open_tables),
      warehouse_id(warehouse_id)
  {
    ALWAYS_ASSERT(warehouse_id == -1 ||
                  (warehouse_id >= 1 &&
                   static_cast<size_t>(warehouse_id) <= NumWarehouses()));
  }

protected:
  virtual void
  load()
  {
    string obj_buf;

    uint64_t order_line_total_sz = 0, n_order_lines = 0;
    uint64_t oorder_total_sz = 0, n_oorders = 0;
    uint64_t new_order_total_sz = 0, n_new_orders = 0;

    const uint w_start = (warehouse_id == -1) ?
      1 : static_cast<uint>(warehouse_id);
    const uint w_end   = (warehouse_id == -1) ?
      NumWarehouses() : static_cast<uint>(warehouse_id);

    for (uint w = w_start; w <= w_end; w++) {
      if (pin_cpus)
        PinToWarehouseId(w);
      for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++) {
        set<uint> c_ids_s;
        while (c_ids_s.size() != NumCustomersPerDistrict())
          c_ids_s.insert((r.next() % NumCustomersPerDistrict()) + 1);
        const vector<uint> c_ids(c_ids_s.begin(), c_ids_s.end());
        for (uint c = 1; c <= NumCustomersPerDistrict();) {
          void * const txn = db->new_txn(txn_flags, txn_buf());
          try {
            const oorder::key k_oo(w, d, c);

            oorder::value v_oo;
            v_oo.o_c_id = c_ids[c - 1];
            if (k_oo.o_id < 2101)
              v_oo.o_carrier_id = RandomNumber(r, 1, 10);
            else
              v_oo.o_carrier_id = 0;
            v_oo.o_ol_cnt = RandomNumber(r, 5, 15);
            v_oo.o_all_local = 1;
            v_oo.o_entry_d = GetCurrentTimeMillis();

            checker::SanityCheckOOrder(&k_oo, &v_oo);
            const size_t sz = Size(v_oo);
            oorder_total_sz += sz;
            n_oorders++;
            tbl_oorder->insert(txn, Encode(k_oo), Encode(obj_buf, v_oo));

            const oorder_c_id_idx::key k_oo_idx(k_oo.o_w_id, k_oo.o_d_id, v_oo.o_c_id, k_oo.o_id);
            const oorder_c_id_idx::value v_oo_idx(0);

            tbl_oorder_c_id_idx->insert(txn, Encode(k_oo_idx), Encode(obj_buf, v_oo_idx));

            if (c >= 2101) {
              const new_order::key k_no(w, d, c);
              const new_order::value v_no(0);

              checker::SanityCheckNewOrder(&k_no, &v_no);
              const size_t sz = Size(v_no);
              new_order_total_sz += sz;
              n_new_orders++;
              tbl_new_order->insert(txn, Encode(k_no), Encode(obj_buf, v_no));
            }

            for (uint l = 1; l <= uint(v_oo.o_ol_cnt); l++) {
              const order_line::key k_ol(w, d, c, l);

              order_line::value v_ol;
              v_ol.ol_i_id = RandomNumber(r, 1, 100000);
              if (k_ol.ol_o_id < 2101) {
                v_ol.ol_delivery_d = v_oo.o_entry_d;
                v_ol.ol_amount = 0;
              } else {
                v_ol.ol_delivery_d = 0;
                // random within [0.01 .. 9,999.99]
                v_ol.ol_amount = (float) (RandomNumber(r, 1, 999999) / 100.0);
              }

              v_ol.ol_supply_w_id = k_ol.ol_w_id;
              v_ol.ol_quantity = 5;
              v_ol.ol_dist_info = RandomStr(r, 24);

              checker::SanityCheckOrderLine(&k_ol, &v_ol);
              const size_t sz = Size(v_ol);
              order_line_total_sz += sz;
              n_order_lines++;
              tbl_order_line->insert(txn, Encode(k_ol), Encode(obj_buf, v_ol));
            }
            if (db->commit_txn(txn)) {
              c++;
            } else {
              db->abort_txn(txn);
              ALWAYS_ASSERT(warehouse_id != -1);
              if (verbose)
                cerr << "[WARNING] order loader loading abort" << endl;
            }
          } catch (abstract_db::abstract_abort_exception &ex) {
            db->abort_txn(txn);
            ALWAYS_ASSERT(warehouse_id != -1);
            if (verbose)
              cerr << "[WARNING] order loader loading abort" << endl;
          }
        }
      }
    }

    if (verbose) {
      if (warehouse_id == -1) {
        cerr << "[INFO] finished loading order" << endl;
        cerr << "[INFO]   * average order_line record length: "
             << (double(order_line_total_sz)/double(n_order_lines)) << " bytes" << endl;
        cerr << "[INFO]   * average oorder record length: "
             << (double(oorder_total_sz)/double(n_oorders)) << " bytes" << endl;
        cerr << "[INFO]   * average new_order record length: "
             << (double(new_order_total_sz)/double(n_new_orders)) << " bytes" << endl;
      } else {
        cerr << "[INFO] finished loading order (w=" << warehouse_id << ")" << endl;
      }
    }
  }

private:
  ssize_t warehouse_id;
};

ssize_t
tpcc_worker::txn_new_order()
{
  const uint districtID = RandomNumber(r, 1, 10);
  const uint customerID = GetCustomerId(r);
  const uint numItems = RandomNumber(r, 5, 15);
  uint itemIDs[15], supplierWarehouseIDs[15], orderQuantities[15];
  bool allLocal = true;
  for (uint i = 0; i < numItems; i++) {
    itemIDs[i] = GetItemId(r);
    if (NumWarehouses() == 1 || RandomNumber(r, 1, 100) > 1) {
      supplierWarehouseIDs[i] = warehouse_id;
    } else {
      do {
       supplierWarehouseIDs[i] = RandomNumber(r, 1, NumWarehouses());
      } while (supplierWarehouseIDs[i] == warehouse_id);
      allLocal = false;
    }
    orderQuantities[i] = RandomNumber(r, 1, 10);
  }

  // XXX(stephentu): implement rollback
  //
  // worst case txn profile:
  //   1 customer get
  //   1 warehouse get
  //   1 district get
  //   1 new_order insert
  //   1 district put
  //   1 oorder insert
  //   1 oorder_cid_idx insert
  //   15 times:
  //      1 item get
  //      1 stock get
  //      1 stock put
  //      1 order_line insert
  //
  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 0
  //   max_read_set_size : 15
  //   max_write_set_size : 15
  //   num_txn_contexts : 9
  void *txn = db->new_txn(txn_flags, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
  scoped_str_arena s_arena(arena);
  try {
    ssize_t ret = 0;
    const customer::key k_c(warehouse_id, districtID, customerID);
    ALWAYS_ASSERT(tbl_customer->get(txn, Encode(obj_key0, k_c), obj_v));
    customer::value v_c_temp;
    const customer::value *v_c = Decode(obj_v, v_c_temp);
    checker::SanityCheckCustomer(&k_c, v_c);

    const warehouse::key k_w(warehouse_id);
    ALWAYS_ASSERT(tbl_warehouse->get(txn, Encode(obj_key0, k_w), obj_v));
    warehouse::value v_w_temp;
    const warehouse::value *v_w = Decode(obj_v, v_w_temp);
    checker::SanityCheckWarehouse(&k_w, v_w);

    const district::key k_d(warehouse_id, districtID);
    ALWAYS_ASSERT(tbl_district->get(txn, Encode(obj_key0, k_d), obj_v));
    district::value v_d_temp;
    const district::value *v_d = Decode(obj_v, v_d_temp);
    checker::SanityCheckDistrict(&k_d, v_d);

    const new_order::key k_no(warehouse_id, districtID, v_d->d_next_o_id);
    const new_order::value v_no(0);
    const size_t new_order_sz = Size(v_no);
    tbl_new_order->insert(txn, Encode(str(), k_no), Encode(str(), v_no));
    ret += new_order_sz;

    district::value v_d_new(*v_d);
    v_d_new.d_next_o_id++;

    tbl_district->put(txn, Encode(str(), k_d), Encode(str(), v_d_new));

    const oorder::key k_oo(warehouse_id, districtID, k_no.no_o_id);
    oorder::value v_oo;
    v_oo.o_c_id = int32_t(customerID);
    v_oo.o_carrier_id = 0; // seems to be ignored
    v_oo.o_ol_cnt = int8_t(numItems);
    v_oo.o_all_local = allLocal;
    v_oo.o_entry_d = GetCurrentTimeMillis();

    const size_t oorder_sz = Size(v_oo);
    tbl_oorder->insert(txn, Encode(str(), k_oo), Encode(str(), v_oo));
    ret += oorder_sz;

    const oorder_c_id_idx::key k_oo_idx(warehouse_id, districtID, customerID, k_no.no_o_id);
    const oorder_c_id_idx::value v_oo_idx(0);

    tbl_oorder_c_id_idx->insert(txn, Encode(str(), k_oo_idx), Encode(str(), v_oo_idx));

    for (uint ol_number = 1; ol_number <= numItems; ol_number++) {
      const uint ol_supply_w_id = supplierWarehouseIDs[ol_number - 1];
      const uint ol_i_id = itemIDs[ol_number - 1];
      const uint ol_quantity = orderQuantities[ol_number - 1];

      const item::key k_i(ol_i_id);
      ALWAYS_ASSERT(tbl_item->get(txn, Encode(obj_key0, k_i), obj_v));
      item::value v_i_temp;
      const item::value *v_i = Decode(obj_v, v_i_temp);
      checker::SanityCheckItem(&k_i, v_i);

      const stock::key k_s(warehouse_id, ol_i_id);
      ALWAYS_ASSERT(tbl_stock->get(txn, Encode(obj_key0, k_s), obj_v));
      stock::value v_s_temp;
      const stock::value *v_s = Decode(obj_v, v_s_temp);
      checker::SanityCheckStock(&k_s, v_s);

      stock::value v_s_new(*v_s);
      if (v_s_new.s_quantity - ol_quantity >= 10)
        v_s_new.s_quantity -= ol_quantity;
      else
        v_s_new.s_quantity += -int32_t(ol_quantity) + 91;
      v_s_new.s_ytd += ol_quantity;
      v_s_new.s_remote_cnt += (ol_supply_w_id == warehouse_id) ? 0 : 1;

      tbl_stock->put(txn, Encode(str(), k_s), Encode(str(), v_s_new));

      const order_line::key k_ol(warehouse_id, districtID, k_no.no_o_id, ol_number);
      order_line::value v_ol;
      v_ol.ol_i_id = int32_t(ol_i_id);
      v_ol.ol_delivery_d = 0; // not delivered yet
      v_ol.ol_amount = float(ol_quantity) * v_i->i_price;
      v_ol.ol_supply_w_id = int32_t(ol_supply_w_id);
      v_ol.ol_quantity = int8_t(ol_quantity);

      const inline_str_fixed<24> *ol_dist_info;
      switch (districtID) {
      case 1:
        ol_dist_info = &v_s->s_dist_01;
        break;
      case 2:
        ol_dist_info = &v_s->s_dist_02;
        break;
      case 3:
        ol_dist_info = &v_s->s_dist_03;
        break;
      case 4:
        ol_dist_info = &v_s->s_dist_04;
        break;
      case 5:
        ol_dist_info = &v_s->s_dist_05;
        break;
      case 6:
        ol_dist_info = &v_s->s_dist_06;
        break;
      case 7:
        ol_dist_info = &v_s->s_dist_07;
        break;
      case 8:
        ol_dist_info = &v_s->s_dist_08;
        break;
      case 9:
        ol_dist_info = &v_s->s_dist_09;
        break;
      case 10:
        ol_dist_info = &v_s->s_dist_10;
        break;
      default:
        ALWAYS_ASSERT(false);
        break;
      }

      NDB_MEMCPY(&v_ol.ol_dist_info, (const char *) ol_dist_info, sizeof(v_ol.ol_dist_info));

      const size_t order_line_sz = Size(v_ol);
      tbl_order_line->insert(txn, Encode(str(), k_ol), Encode(str(), v_ol));
      ret += order_line_sz;
    }

    measure_txn_counters(txn, "txn_new_order");
    if (db->commit_txn(txn)) {
      ntxn_commits++;
      return ret;
    } else {
      ntxn_aborts++;
    }
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
    ntxn_aborts++;
  }
  return 0;
}

class new_order_scan_callback : public abstract_ordered_index::scan_callback {
public:
  new_order_scan_callback() : k_no(0) {}
  virtual bool invoke(
      const string &key,
      const string &value)
  {
    INVARIANT(key.size() == sizeof(new_order::key));
    INVARIANT(value.size() == 1);
    k_no = Decode(key, k_no_temp);
#ifdef CHECK_INVARIANTS
    new_order::value v_no_temp;
    const new_order::value *v_no = Decode(value, v_no_temp);
    checker::SanityCheckNewOrder(k_no, v_no);
#endif
    return false;
  }
  inline const new_order::key *
  get_key() const
  {
    return k_no;
  }
private:
  new_order::key k_no_temp;
  const new_order::key *k_no;
};

STATIC_COUNTER_DECL(scopedperf::tod_ctr, delivery_probe0_tod, delivery_probe0_cg)

ssize_t
tpcc_worker::txn_delivery()
{
  const uint o_carrier_id = RandomNumber(r, 1, NumDistrictsPerWarehouse());
  const uint32_t ts = GetCurrentTimeMillis();

  // worst case txn profile:
  //   10 times:
  //     1 new_order scan node
  //     1 oorder get
  //     2 order_line scan nodes
  //     15 order_line puts
  //     1 new_order remove
  //     1 oorder put
  //     1 customer get
  //     1 customer put
  //
  // output from counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 21
  //   max_read_set_size : 133
  //   max_write_set_size : 133
  //   num_txn_contexts : 4
  void *txn = db->new_txn(txn_flags, txn_buf(), abstract_db::HINT_TPCC_DELIVERY);
  scoped_str_arena s_arena(arena);
  try {
    ssize_t ret = 0;
    for (uint d = 1; d <= NumDistrictsPerWarehouse(); d++) {
      const new_order::key k_no_0(warehouse_id, d, last_no_o_ids[d]);
      const new_order::key k_no_1(warehouse_id, d, numeric_limits<int32_t>::max());
      new_order_scan_callback new_order_c;
      {
        ANON_REGION("DeliverNewOrderScan:", &delivery_probe0_cg);
        tbl_new_order->scan(txn, Encode(obj_key0, k_no_0), &Encode(obj_key1, k_no_1), new_order_c, s_arena.get());
      }

      const new_order::key *k_no = new_order_c.get_key();
      if (unlikely(!k_no))
        continue;
      last_no_o_ids[d] = k_no->no_o_id + 1; // XXX: update last seen

      const oorder::key k_oo(warehouse_id, d, k_no->no_o_id);
      ALWAYS_ASSERT(tbl_oorder->get(txn, Encode(obj_key0, k_oo), obj_v));
      oorder::value v_oo_temp;
      const oorder::value *v_oo = Decode(obj_v, v_oo_temp);
      checker::SanityCheckOOrder(&k_oo, v_oo);

      static_limit_callback<15> c(s_arena.get()); // never more than 15 order_lines per order
      const order_line::key k_oo_0(warehouse_id, d, k_no->no_o_id, 0);
      const order_line::key k_oo_1(warehouse_id, d, k_no->no_o_id, numeric_limits<int32_t>::max());

      // XXX(stephentu): mutable scans would help here
      tbl_order_line->scan(txn, Encode(obj_key0, k_oo_0), &Encode(obj_key1, k_oo_1), c, s_arena.get());
      float sum = 0.0;
      for (size_t i = 0; i < c.size(); i++) {
        order_line::value v_ol_temp;
        const order_line::value *v_ol = Decode(c.values[i].second, v_ol_temp);

#ifdef CHECK_INVARIANTS
        order_line::key k_ol_temp;
        const order_line::key *k_ol = Decode(c.values[i].first, k_ol_temp);
        checker::SanityCheckOrderLine(k_ol, v_ol);
#endif

        sum += v_ol->ol_amount;
        order_line::value v_ol_new(*v_ol);
        v_ol_new.ol_delivery_d = ts;
        tbl_order_line->put(txn, c.values[i].first, Encode(str(), v_ol_new));
      }

      // delete new order
      tbl_new_order->remove(txn, Encode(str(), *k_no));
      ret -= 0 /*new_order_c.get_value_size()*/;

      // update oorder
      oorder::value v_oo_new(*v_oo);
      v_oo_new.o_carrier_id = o_carrier_id;
      tbl_oorder->put(txn, Encode(str(), k_oo), Encode(str(), v_oo_new));

      const uint c_id = v_oo->o_c_id;
      const float ol_total = sum;

      // update customer
      const customer::key k_c(warehouse_id, d, c_id);
      ALWAYS_ASSERT(tbl_customer->get(txn, Encode(obj_key0, k_c), obj_v));

      customer::value v_c_temp;
      const customer::value *v_c = Decode(obj_v, v_c_temp);
      customer::value v_c_new(*v_c);
      v_c_new.c_balance += ol_total;
      tbl_customer->put(txn, Encode(str(), k_c), Encode(str(), v_c_new));
    }
    measure_txn_counters(txn, "txn_delivery");
    if (db->commit_txn(txn)) {
      ntxn_commits++;
      return ret;
    } else {
      ntxn_aborts++;
    }
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
    ntxn_aborts++;
  }
  return 0;
}

ssize_t
tpcc_worker::txn_payment()
{
  const uint districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
  uint customerDistrictID, customerWarehouseID;
  if (NumWarehouses() == 1 || RandomNumber(r, 1, 100) <= 85) {
    customerDistrictID = districtID;
    customerWarehouseID = warehouse_id;
  } else {
    customerDistrictID = RandomNumber(r, 1, NumDistrictsPerWarehouse());
    do {
      customerWarehouseID = RandomNumber(r, 1, NumWarehouses());
    } while (customerWarehouseID == warehouse_id);
  }
  const float paymentAmount = (float) (RandomNumber(r, 100, 500000) / 100.0);
  const uint32_t ts = GetCurrentTimeMillis();

  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 10
  //   max_read_set_size : 71
  //   max_write_set_size : 1
  //   num_txn_contexts : 5
  void *txn = db->new_txn(txn_flags, txn_buf(), abstract_db::HINT_TPCC_PAYMENT);
  scoped_str_arena s_arena(arena);
  try {
    ssize_t ret = 0;

    const warehouse::key k_w(warehouse_id);
    ALWAYS_ASSERT(tbl_warehouse->get(txn, Encode(obj_key0, k_w), obj_v));
    warehouse::value v_w_temp;
    const warehouse::value *v_w = Decode(obj_v, v_w_temp);
    checker::SanityCheckWarehouse(&k_w, v_w);

    warehouse::value v_w_new(*v_w);
    v_w_new.w_ytd += paymentAmount;
    tbl_warehouse->put(txn, Encode(str(), k_w), Encode(str(), v_w_new));

    const district::key k_d(warehouse_id, districtID);
    ALWAYS_ASSERT(tbl_district->get(txn, Encode(obj_key0, k_d), obj_v));
    district::value v_d_temp;
    const district::value *v_d = Decode(obj_v, v_d_temp);
    checker::SanityCheckDistrict(&k_d, v_d);

    district::value v_d_new(*v_d);
    v_d_new.d_ytd += paymentAmount;
    tbl_district->put(txn, Encode(str(), k_d), Encode(str(), v_d_new));

    customer::key k_c;
    customer::value v_c;
    if (RandomNumber(r, 1, 100) <= 60) {
      // cust by name
      uint8_t lastname_buf[CustomerLastNameMaxSize + 1];
      _static_assert(sizeof(lastname_buf) == 16);
      NDB_MEMSET(lastname_buf, 0, sizeof(lastname_buf));
      GetNonUniformCustomerLastNameRun(lastname_buf, r);

      static const string zeros(16, 0);
      static const string ones(16, 255);

      customer_name_idx::key k_c_idx_0;
      k_c_idx_0.c_w_id = customerWarehouseID;
      k_c_idx_0.c_d_id = customerDistrictID;
      k_c_idx_0.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_0.c_first.assign(zeros);

      customer_name_idx::key k_c_idx_1;
      k_c_idx_1.c_w_id = customerWarehouseID;
      k_c_idx_1.c_d_id = customerDistrictID;
      k_c_idx_1.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_1.c_first.assign(ones);

      static_limit_callback<NMaxCustomerIdxScanElems> c(s_arena.get()); // probably a safe bet for now
      tbl_customer_name_idx->scan(txn, Encode(obj_key0, k_c_idx_0), &Encode(obj_key1, k_c_idx_1), c, s_arena.get());
      INVARIANT(c.size() > 0);
      INVARIANT(c.size() < NMaxCustomerIdxScanElems); // we should detect this
      int index = c.size() / 2;
      if (c.size() % 2 == 0)
        index--;

      customer_name_idx::value v_c_idx_temp;
      const customer_name_idx::value *v_c_idx = Decode(c.values[index].second, v_c_idx_temp);

      k_c.c_w_id = customerWarehouseID;
      k_c.c_d_id = customerDistrictID;
      k_c.c_id = v_c_idx->c_id;
      ALWAYS_ASSERT(tbl_customer->get(txn, Encode(obj_key0, k_c), obj_v));
      Decode(obj_v, v_c);

    } else {
      // cust by ID
      const uint customerID = GetCustomerId(r);
      k_c.c_w_id = customerWarehouseID;
      k_c.c_d_id = customerDistrictID;
      k_c.c_id = customerID;
      ALWAYS_ASSERT(tbl_customer->get(txn, Encode(obj_key0, k_c), obj_v));
      Decode(obj_v, v_c);
    }
    checker::SanityCheckCustomer(&k_c, &v_c);
    customer::value v_c_new(v_c);

    v_c_new.c_balance -= paymentAmount;
    v_c_new.c_ytd_payment += paymentAmount;
    v_c_new.c_payment_cnt++;
    if (strncmp(v_c.c_credit.data(), "BC", 2) == 0) {
      char buf[501];
      int n = snprintf(buf, sizeof(buf), "%d %d %d %d %d %f | %s",
                       k_c.c_id,
                       k_c.c_d_id,
                       k_c.c_w_id,
                       districtID,
                       warehouse_id,
                       paymentAmount,
                       v_c.c_data.c_str());
      v_c_new.c_data.resize_junk(
          min(static_cast<size_t>(n), v_c_new.c_data.max_size()));
      NDB_MEMCPY((void *) v_c_new.c_data.data(), &buf[0], v_c_new.c_data.size());
    }

    tbl_customer->put(txn, Encode(str(), k_c), Encode(str(), v_c_new));

    const history::key k_h(k_c.c_d_id, k_c.c_w_id, k_c.c_id, districtID, warehouse_id, ts);
    history::value v_h;
    v_h.h_amount = paymentAmount;
    v_h.h_data.resize_junk(v_h.h_data.max_size());
    int n = snprintf((char *) v_h.h_data.data(), v_h.h_data.max_size() + 1,
                     "%.10s    %.10s",
                     v_w->w_name.c_str(),
                     v_d->d_name.c_str());
    v_h.h_data.resize_junk(min(static_cast<size_t>(n), v_h.h_data.max_size()));

    const size_t history_sz = Size(v_h);
    tbl_history->insert(txn, Encode(str(), k_h), Encode(str(), v_h));
    ret += history_sz;

    measure_txn_counters(txn, "txn_payment");
    if (db->commit_txn(txn)) {
      ntxn_commits++;
      return ret;
    } else {
      ntxn_aborts++;
    }
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
    ntxn_aborts++;
  }
  return 0;
}

class order_line_nop_callback : public abstract_ordered_index::scan_callback {
public:
  order_line_nop_callback() : n(0) {}
  virtual bool invoke(
      const string &key,
      const string &value)
  {
    INVARIANT(key.size() == sizeof(order_line::key));
    order_line::value v_ol_temp;
    const order_line::value *v_ol UNUSED = Decode(value, v_ol_temp);
#ifdef CHECK_INVARIANTS
    order_line::key k_ol_temp;
    const order_line::key *k_ol = Decode(key, k_ol_temp);
    checker::SanityCheckOrderLine(k_ol, v_ol);
#endif
    ++n;
    return true;
  }
  size_t n;
};

STATIC_COUNTER_DECL(scopedperf::tod_ctr, order_status_probe0_tod, order_status_probe0_cg)
static event_avg_counter evt_avg_order_status_oorder_scan_size("avg_order_status_oorder_scan_size");

ssize_t
tpcc_worker::txn_order_status()
{
  const uint districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());

  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 13
  //   max_read_set_size : 81
  //   max_write_set_size : 0
  //   num_txn_contexts : 4
  void *txn = db->new_txn(txn_flags | transaction_base::TXN_FLAG_READ_ONLY, txn_buf(), abstract_db::HINT_TPCC_ORDER_STATUS);
  scoped_str_arena s_arena(arena);
  try {

    customer::key k_c;
    customer::value v_c;
    if (RandomNumber(r, 1, 100) <= 60) {
      // cust by name
      uint8_t lastname_buf[CustomerLastNameMaxSize + 1];
      _static_assert(sizeof(lastname_buf) == 16);
      NDB_MEMSET(lastname_buf, 0, sizeof(lastname_buf));
      GetNonUniformCustomerLastNameRun(lastname_buf, r);

      static const string zeros(16, 0);
      static const string ones(16, 255);

      customer_name_idx::key k_c_idx_0;
      k_c_idx_0.c_w_id = warehouse_id;
      k_c_idx_0.c_d_id = districtID;
      k_c_idx_0.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_0.c_first.assign(zeros);

      customer_name_idx::key k_c_idx_1;
      k_c_idx_1.c_w_id = warehouse_id;
      k_c_idx_1.c_d_id = districtID;
      k_c_idx_1.c_last.assign((const char *) lastname_buf, 16);
      k_c_idx_1.c_first.assign(ones);

      static_limit_callback<NMaxCustomerIdxScanElems> c(s_arena.get()); // probably a safe bet for now
      tbl_customer_name_idx->scan(txn, Encode(obj_key0, k_c_idx_0), &Encode(obj_key1, k_c_idx_1), c, s_arena.get());
      INVARIANT(c.size() > 0);
      INVARIANT(c.size() < NMaxCustomerIdxScanElems); // we should detect this
      int index = c.size() / 2;
      if (c.size() % 2 == 0)
        index--;

      customer_name_idx::value v_c_idx_temp;
      const customer_name_idx::value *v_c_idx = Decode(c.values[index].second, v_c_idx_temp);

      k_c.c_w_id = warehouse_id;
      k_c.c_d_id = districtID;
      k_c.c_id = v_c_idx->c_id;
      ALWAYS_ASSERT(tbl_customer->get(txn, Encode(obj_key0, k_c), obj_v));
      Decode(obj_v, v_c);

    } else {
      // cust by ID
      const uint customerID = GetCustomerId(r);
      k_c.c_w_id = warehouse_id;
      k_c.c_d_id = districtID;
      k_c.c_id = customerID;
      ALWAYS_ASSERT(tbl_customer->get(txn, Encode(obj_key0, k_c), obj_v));
      Decode(obj_v, v_c);
    }
    checker::SanityCheckCustomer(&k_c, &v_c);

    // XXX: store last value from client so we don't have to scan
    // from the beginning
    latest_key_callback c_oorder(str());
    const oorder_c_id_idx::key k_oo_idx_0(warehouse_id, districtID, k_c.c_id, 0);
    const oorder_c_id_idx::key k_oo_idx_1(warehouse_id, districtID, k_c.c_id, numeric_limits<int32_t>::max());
    {
      ANON_REGION("OrderStatusOOrderScan:", &order_status_probe0_cg);
      tbl_oorder_c_id_idx->scan(txn, Encode(obj_key0, k_oo_idx_0), &Encode(obj_key1, k_oo_idx_1), c_oorder, s_arena.get());
    }
    INVARIANT(c_oorder.size());
    evt_avg_order_status_oorder_scan_size.offer(c_oorder.size());

    oorder_c_id_idx::key k_oo_idx_temp;
    const oorder_c_id_idx::key *k_oo_idx = Decode(c_oorder.kstr(), k_oo_idx_temp);
    const uint o_id = k_oo_idx->o_o_id;

    order_line_nop_callback c_order_line;
    const order_line::key k_ol_0(warehouse_id, districtID, o_id, 0);
    const order_line::key k_ol_1(warehouse_id, districtID, o_id, numeric_limits<int32_t>::max());
    tbl_order_line->scan(txn, Encode(obj_key0, k_ol_0), &Encode(obj_key1, k_ol_1), c_order_line, s_arena.get());
    INVARIANT(c_order_line.n >= 5 && c_order_line.n <= 15);

    measure_txn_counters(txn, "txn_order_status");
    if (db->commit_txn(txn))
      ntxn_commits++;
    else
      ntxn_aborts++;
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
    ntxn_aborts++;
  }
  return 0;
}

class order_line_scan_callback : public abstract_ordered_index::scan_callback {
public:
  order_line_scan_callback() : n(0) {}
  virtual bool invoke(
      const string &key,
      const string &value)
  {
    INVARIANT(key.size() == sizeof(order_line::key));
    order_line::value v_ol_temp;
    const order_line::value *v_ol = Decode(value, v_ol_temp);

#ifdef CHECK_INVARIANTS
    order_line::key k_ol_temp;
    const order_line::key *k_ol = Decode(key, k_ol_temp);
    checker::SanityCheckOrderLine(k_ol, v_ol);
#endif

    s_i_ids[v_ol->ol_i_id] = 1;
    n++;
    return true;
  }
  size_t n;
  small_unordered_map<uint, bool, 256> s_i_ids;
};

STATIC_COUNTER_DECL(scopedperf::tod_ctr, stock_level_probe0_tod, stock_level_probe0_cg)
STATIC_COUNTER_DECL(scopedperf::tod_ctr, stock_level_probe1_tod, stock_level_probe1_cg)
STATIC_COUNTER_DECL(scopedperf::tod_ctr, stock_level_probe2_tod, stock_level_probe2_cg)

static event_avg_counter evt_avg_stock_level_loop_join_lookups("stock_level_loop_join_lookups");

ssize_t
tpcc_worker::txn_stock_level()
{
  const uint threshold = RandomNumber(r, 10, 20);
  const uint districtID = RandomNumber(r, 1, NumDistrictsPerWarehouse());

  // output from txn counters:
  //   max_absent_range_set_size : 0
  //   max_absent_set_size : 0
  //   max_node_scan_size : 19
  //   max_read_set_size : 241
  //   max_write_set_size : 0
  //   n_node_scan_large_instances : 1
  //   n_read_set_large_instances : 2
  //   num_txn_contexts : 3
  void *txn = db->new_txn(txn_flags | transaction_base::TXN_FLAG_READ_ONLY, txn_buf(), abstract_db::HINT_TPCC_STOCK_LEVEL);
  scoped_str_arena s_arena(arena);
  try {
    const district::key k_d(warehouse_id, districtID);
    ALWAYS_ASSERT(tbl_district->get(txn, Encode(obj_key0, k_d), obj_v));
    district::value v_d_temp;
    const district::value *v_d = Decode(obj_v, v_d_temp);
    checker::SanityCheckDistrict(&k_d, v_d);

    // manual joins are fun!
    order_line_scan_callback c;
    const int32_t lower = v_d->d_next_o_id >= 20 ? (v_d->d_next_o_id - 20) : 0;
    const order_line::key k_ol_0(warehouse_id, districtID, lower, 0);
    const order_line::key k_ol_1(warehouse_id, districtID, v_d->d_next_o_id, 0);
    {
      ANON_REGION("StockLevelOrderLineScan:", &stock_level_probe0_cg);
      tbl_order_line->scan(txn, Encode(obj_key0, k_ol_0), &Encode(obj_key1, k_ol_1), c, s_arena.get());
    }
    {
      small_unordered_map<uint, bool, 256> s_i_ids_distinct;
      for (auto &p : c.s_i_ids) {
        ANON_REGION("StockLevelLoopJoinIter:", &stock_level_probe1_cg);

        const serializer<int16_t, true> i16s;
        const size_t nbytesread = i16s.max_nbytes();

        const stock::key k_s(warehouse_id, p.first);
        INVARIANT(p.first >= 1 && p.first <= NumItems());
        {
          ANON_REGION("StockLevelLoopJoinGet:", &stock_level_probe2_cg);
          ALWAYS_ASSERT(tbl_stock->get(txn, Encode(obj_key0, k_s), obj_v, nbytesread));
        }
        INVARIANT(obj_v.size() <= nbytesread);
        const uint8_t *ptr = (const uint8_t *) obj_v.data();
        int16_t i16tmp;
        ptr = i16s.read(ptr, &i16tmp);
        if (i16tmp < int(threshold))
          s_i_ids_distinct[p.first] = 1;
      }
      evt_avg_stock_level_loop_join_lookups.offer(c.s_i_ids.size());
      // NB(stephentu): s_i_ids_distinct.size() is the computed result of this txn
    }
    measure_txn_counters(txn, "txn_stock_level");
    if (db->commit_txn(txn))
      ntxn_commits++;
    else
      ntxn_aborts++;
  } catch (abstract_db::abstract_abort_exception &ex) {
    db->abort_txn(txn);
    ntxn_aborts++;
  }
  return 0;
}

class tpcc_bench_runner : public bench_runner {
public:
  tpcc_bench_runner(abstract_db *db)
    : bench_runner(db)
  {
    open_tables["customer"]          = db->open_index("customer", sizeof(customer));
    open_tables["customer_name_idx"] = db->open_index("customer_name_idx", sizeof(customer_name_idx));
    open_tables["district"]          = db->open_index("district", sizeof(district));
    open_tables["history"]           = db->open_index("history", sizeof(history), true);
    open_tables["item"]              = db->open_index("item", sizeof(item));
    open_tables["new_order"]         = db->open_index("new_order", sizeof(new_order));
    open_tables["oorder"]            = db->open_index("oorder", sizeof(oorder));
    open_tables["oorder_c_id_idx"]   = db->open_index("oorder_c_id_idx", sizeof(oorder_c_id_idx), true);
    open_tables["order_line"]        = db->open_index("order_line", sizeof(order_line));
    open_tables["stock"]             = db->open_index("stock", sizeof(stock));
    open_tables["warehouse"]         = db->open_index("warehouse", sizeof(warehouse));
  }

protected:
  virtual vector<bench_loader *>
  make_loaders()
  {
    vector<bench_loader *> ret;
    ret.push_back(new tpcc_warehouse_loader(9324, db, open_tables));
    ret.push_back(new tpcc_item_loader(235443, db, open_tables));
    if (enable_parallel_loading) {
      fast_random r(89785943);
      for (uint i = 1; i <= NumWarehouses(); i++)
        ret.push_back(new tpcc_stock_loader(r.next(), db, open_tables, i));
    } else {
      ret.push_back(new tpcc_stock_loader(89785943, db, open_tables, -1));
    }
    ret.push_back(new tpcc_district_loader(129856349, db, open_tables));
    if (enable_parallel_loading) {
      fast_random r(923587856425);
      for (uint i = 1; i <= NumWarehouses(); i++)
        ret.push_back(new tpcc_customer_loader(r.next(), db, open_tables, i));
    } else {
      ret.push_back(new tpcc_customer_loader(923587856425, db, open_tables, -1));
    }
    if (enable_parallel_loading) {
      fast_random r(2343352);
      for (uint i = 1; i <= NumWarehouses(); i++)
        ret.push_back(new tpcc_order_loader(r.next(), db, open_tables, i));
    } else {
      ret.push_back(new tpcc_order_loader(2343352, db, open_tables, -1));
    }
    return ret;
  }

  virtual vector<bench_worker *>
  make_workers()
  {
    fast_random r(23984543);
    vector<bench_worker *> ret;
    for (size_t i = 0; i < nthreads; i++)
      ret.push_back(
        new tpcc_worker(
          i, r.next(), db, open_tables,
          &barrier_a, &barrier_b,
          (i % NumWarehouses()) + 1));
    return ret;
  }
};

void
tpcc_do_test(abstract_db *db)
{
  tpcc_bench_runner r(db);
  r.run();
}
