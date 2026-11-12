// Python extension module exposing the C++ OrderBook.
//
// Written against the CPython C API directly (no pybind11), so building it
// needs only a C++ compiler and Python's headers:  make -C cpp python
//
//   import obfast
//   book = obfast.Book()
//   book.limit(1, obfast.SELL, 100, 10_050)        # -> []
//   book.limit(2, obfast.BUY, 40, 10_100)          # -> [(2, 1, 10050, 40, 0)]
//   book.best_ask(), book.mid(), book.depth(obfast.SELL)
//
// Trades come back as tuples: (buy_id, sell_id, price, qty, aggressor_side).
// Hot methods use METH_FASTCALL so a call costs little more than the C++ work.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "orderbook.hpp"

#include <vector>

namespace {

PyObject* g_error = nullptr;  // obfast.OrderBookError (subclass of ValueError)

struct BookObject {
    PyObject_HEAD
    ob::OrderBook* book;
    std::vector<ob::Trade>* out;  // reused buffer for trades
};

// ---------- argument helpers ----------

bool to_u64(PyObject* o, std::uint64_t& v) {
    v = PyLong_AsUnsignedLongLong(o);
    return !(v == std::uint64_t(-1) && PyErr_Occurred());
}

bool to_i64(PyObject* o, std::int64_t& v) {
    v = PyLong_AsLongLong(o);
    return !(v == -1 && PyErr_Occurred());
}

bool to_side(PyObject* o, ob::Side& s) {
    const long v = PyLong_AsLong(o);
    if (v == -1 && PyErr_Occurred()) return false;
    if (v != 0 && v != 1) {
        PyErr_SetString(PyExc_ValueError, "side must be obfast.BUY (0) or obfast.SELL (1)");
        return false;
    }
    s = v == 0 ? ob::Side::Buy : ob::Side::Sell;
    return true;
}

bool check_nargs(Py_ssize_t nargs, Py_ssize_t want) {
    if (nargs == want) return true;
    PyErr_Format(PyExc_TypeError, "expected %zd arguments, got %zd", want, nargs);
    return false;
}

PyObject* trades_to_list(const std::vector<ob::Trade>& trades) {
    PyObject* list = PyList_New(Py_ssize_t(trades.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < trades.size(); ++i) {
        const ob::Trade& t = trades[i];
        PyObject* tup = Py_BuildValue("(KKLLi)", (unsigned long long)t.buy_id, (unsigned long long)t.sell_id,
                                      (long long)t.price, (long long)t.qty, t.aggressor == ob::Side::Buy ? 0 : 1);
        if (!tup) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, Py_ssize_t(i), tup);
    }
    return list;
}

PyObject* price_or_none(std::optional<ob::Price> p) {
    if (!p) Py_RETURN_NONE;
    return PyLong_FromLongLong(*p);
}

// Runs fn(); turns C++ exceptions into Python exceptions. Returns false on error.
template <class Fn>
bool guarded(Fn fn) {
    try {
        fn();
        return true;
    } catch (const ob::OrderBookError& e) {
        PyErr_SetString(g_error, e.what());
    } catch (const std::bad_alloc&) {
        PyErr_NoMemory();
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
    }
    return false;
}

// ---------- Book type ----------

PyObject* Book_new(PyTypeObject* type, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"max_levels", nullptr};
    Py_ssize_t max_levels = Py_ssize_t(ob::OrderBook::kDefaultMaxLevels);
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|n", const_cast<char**>(kwlist), &max_levels)) return nullptr;
    if (max_levels <= 0) {
        PyErr_SetString(PyExc_ValueError, "max_levels must be positive");
        return nullptr;
    }
    auto* self = reinterpret_cast<BookObject*>(type->tp_alloc(type, 0));
    if (!self) return nullptr;
    if (!guarded([&] {
            self->book = new ob::OrderBook(std::size_t(max_levels));
            self->book->keep_history(false);  // trades are returned per call instead
            self->out = new std::vector<ob::Trade>();
            self->out->reserve(64);
        })) {
        Py_DECREF(self);
        return nullptr;
    }
    return reinterpret_cast<PyObject*>(self);
}

void Book_dealloc(BookObject* self) {
    delete self->book;
    delete self->out;
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

template <ob::OrderType Type>
PyObject* Book_add(BookObject* self, PyObject* const* args, Py_ssize_t nargs) {
    constexpr bool kHasPrice = Type != ob::OrderType::Market;
    if (!check_nargs(nargs, kHasPrice ? 4 : 3)) return nullptr;
    ob::Order o{};
    o.type = Type;
    if (!to_u64(args[0], o.id) || !to_side(args[1], o.side) || !to_i64(args[2], o.qty)) return nullptr;
    if (kHasPrice && !to_i64(args[3], o.price)) return nullptr;
    o.remaining = o.qty;
    self->out->clear();
    if (!guarded([&] { self->book->add(o, *self->out); })) return nullptr;
    return trades_to_list(*self->out);
}

PyObject* Book_cancel(BookObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (!check_nargs(nargs, 1)) return nullptr;
    std::uint64_t id;
    if (!to_u64(args[0], id)) return nullptr;
    return PyBool_FromLong(self->book->cancel(id));
}

PyObject* Book_modify(BookObject* self, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"id", "qty", "price", nullptr};
    unsigned long long id;
    PyObject* qty_obj = Py_None;
    PyObject* price_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "K|OO", const_cast<char**>(kwlist), &id, &qty_obj, &price_obj))
        return nullptr;
    std::optional<ob::Qty> qty;
    std::optional<ob::Price> price;
    std::int64_t v;
    if (qty_obj != Py_None) {
        if (!to_i64(qty_obj, v)) return nullptr;
        qty = v;
    }
    if (price_obj != Py_None) {
        if (!to_i64(price_obj, v)) return nullptr;
        price = v;
    }
    self->out->clear();
    if (!guarded([&] { self->book->modify(id, qty, price, *self->out); })) return nullptr;
    return trades_to_list(*self->out);
}

PyObject* Book_best_bid(BookObject* self, PyObject*) { return price_or_none(self->book->best_bid()); }
PyObject* Book_best_ask(BookObject* self, PyObject*) { return price_or_none(self->book->best_ask()); }
PyObject* Book_spread(BookObject* self, PyObject*) { return price_or_none(self->book->spread()); }

PyObject* Book_mid(BookObject* self, PyObject*) {
    auto b = self->book->best_bid(), a = self->book->best_ask();
    if (!b || !a) Py_RETURN_NONE;
    return PyFloat_FromDouble((double(*b) + double(*a)) / 2.0);
}

PyObject* Book_depth(BookObject* self, PyObject* args) {
    int side_int;
    Py_ssize_t levels = 5;
    if (!PyArg_ParseTuple(args, "i|n", &side_int, &levels)) return nullptr;
    if (side_int != 0 && side_int != 1) {
        PyErr_SetString(PyExc_ValueError, "side must be obfast.BUY (0) or obfast.SELL (1)");
        return nullptr;
    }
    if (levels < 0) levels = 0;
    const auto d = self->book->depth(side_int == 0 ? ob::Side::Buy : ob::Side::Sell, std::size_t(levels));
    PyObject* list = PyList_New(Py_ssize_t(d.size()));
    if (!list) return nullptr;
    for (std::size_t i = 0; i < d.size(); ++i) {
        PyObject* tup = Py_BuildValue("(LL)", (long long)d[i].first, (long long)d[i].second);
        if (!tup) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, Py_ssize_t(i), tup);
    }
    return list;
}

PyObject* Book_remaining(BookObject* self, PyObject* const* args, Py_ssize_t nargs) {
    if (!check_nargs(nargs, 1)) return nullptr;
    std::uint64_t id;
    if (!to_u64(args[0], id)) return nullptr;
    const ob::Order* o = self->book->get(id);
    if (!o) Py_RETURN_NONE;
    return PyLong_FromLongLong(o->remaining);
}

PyObject* Book_resting_qty(BookObject* self, PyObject*) { return PyLong_FromLongLong(self->book->resting_qty()); }

Py_ssize_t Book_len(BookObject* self) { return Py_ssize_t(self->book->size()); }

PyObject* Book_repr(BookObject* self) {
    std::string s;
    if (!guarded([&] { s = self->book->to_string(); })) return nullptr;
    return PyUnicode_FromStringAndSize(s.data(), Py_ssize_t(s.size()));
}

#define FASTCALL(fn) reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(fn))

PyMethodDef Book_methods[] = {
    {"limit", FASTCALL(Book_add<ob::OrderType::Limit>), METH_FASTCALL,
     "limit(id, side, qty, price) -> list of trades. Leftover rests in the book."},
    {"ioc", FASTCALL(Book_add<ob::OrderType::IOC>), METH_FASTCALL,
     "ioc(id, side, qty, price) -> list of trades. Leftover is dropped."},
    {"market", FASTCALL(Book_add<ob::OrderType::Market>), METH_FASTCALL,
     "market(id, side, qty) -> list of trades. Leftover is dropped."},
    {"cancel", FASTCALL(Book_cancel), METH_FASTCALL, "cancel(id) -> True if the order was resting."},
    {"modify", FASTCALL(Book_modify), METH_VARARGS | METH_KEYWORDS,
     "modify(id, qty=None, price=None) -> list of trades. Shrinking keeps queue position."},
    {"best_bid", FASTCALL(Book_best_bid), METH_NOARGS, "Best bid price, or None."},
    {"best_ask", FASTCALL(Book_best_ask), METH_NOARGS, "Best ask price, or None."},
    {"spread", FASTCALL(Book_spread), METH_NOARGS, "best_ask - best_bid, or None."},
    {"mid", FASTCALL(Book_mid), METH_NOARGS, "(best_bid + best_ask) / 2 as a float, or None."},
    {"depth", FASTCALL(Book_depth), METH_VARARGS, "depth(side, levels=5) -> [(price, total_qty), ...] best first."},
    {"remaining", FASTCALL(Book_remaining), METH_FASTCALL, "remaining(id) -> resting quantity, or None."},
    {"resting_qty", FASTCALL(Book_resting_qty), METH_NOARGS, "Total quantity resting in the book."},
    {nullptr, nullptr, 0, nullptr},
};

PySequenceMethods Book_as_sequence = {};
PyTypeObject BookType = {PyVarObject_HEAD_INIT(nullptr, 0)};

PyModuleDef obfast_module = {PyModuleDef_HEAD_INIT};

}  // namespace

PyMODINIT_FUNC PyInit_obfast(void) {
    Book_as_sequence.sq_length = reinterpret_cast<lenfunc>(Book_len);

    BookType.tp_name = "obfast.Book";
    BookType.tp_basicsize = sizeof(BookObject);
    BookType.tp_flags = Py_TPFLAGS_DEFAULT;
    BookType.tp_doc = "Limit order book with price-time priority (C++ implementation).\n\n"
                      "Book(max_levels=4194304). Prices are integer ticks; ids must be unique among resting orders.";
    BookType.tp_new = Book_new;
    BookType.tp_dealloc = reinterpret_cast<destructor>(Book_dealloc);
    BookType.tp_methods = Book_methods;
    BookType.tp_as_sequence = &Book_as_sequence;
    BookType.tp_repr = reinterpret_cast<reprfunc>(Book_repr);
    if (PyType_Ready(&BookType) < 0) return nullptr;

    obfast_module.m_name = "obfast";
    obfast_module.m_doc = "Fast C++ limit order book for Python.";
    obfast_module.m_size = -1;
    PyObject* m = PyModule_Create(&obfast_module);
    if (!m) return nullptr;

    Py_INCREF(&BookType);
    if (PyModule_AddObject(m, "Book", reinterpret_cast<PyObject*>(&BookType)) < 0) {
        Py_DECREF(&BookType);
        Py_DECREF(m);
        return nullptr;
    }
    g_error = PyErr_NewException("obfast.OrderBookError", PyExc_ValueError, nullptr);
    if (!g_error || PyModule_AddObject(m, "OrderBookError", g_error) < 0) {
        Py_XDECREF(g_error);
        Py_DECREF(m);
        return nullptr;
    }
    Py_INCREF(g_error);  // module keeps one reference, we keep one
    PyModule_AddIntConstant(m, "BUY", 0);
    PyModule_AddIntConstant(m, "SELL", 1);
    return m;
}
