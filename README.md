# PY-ADV-02 — Mini E-Commerce Order System

A small, runnable Python application built to exercise advanced OOP,
custom exceptions, and design patterns. Domain: a customer builds a
`Cart` of `Product`s and places an `Order`, which reserves stock from a
shared `InventoryManager`.

## Running it

```bash
python3 main.py                        # demo script
python3 -m unittest test_system.py -v  # unit tests
```

## File map

```
ecommerce_system/
├── exceptions.py        # custom exception hierarchy
├── models.py             # Product and its subclasses
├── patterns/
│   ├── factory.py         # Factory pattern
│   ├── singleton.py       # Singleton pattern
│   └── strategy.py        # Strategy pattern
├── context_managers.py    # custom + contextlib context managers
├── order.py               # Cart, Order (class/static methods)
├── main.py                # wiring / demo
└── test_system.py         # unit tests
```

## How each requirement is met

### 1. OOP fundamentals
- **Classes & objects**: `Product`, `Cart`, `Order`, `InventoryManager`, etc.
- **Inheritance**: `PhysicalProduct`, `DigitalProduct`, `SubscriptionProduct`
  all inherit from `Product`, reusing its validation, equality, and
  hashing logic instead of duplicating it.
- **Abstraction**: `Product` is an `ABC` with abstract methods
  `calculate_shipping_cost()` and `get_category()`. Client code (`Cart`,
  `main.py`) works entirely against that interface and never branches on
  concrete subclass.
- **Encapsulation**: `Product.price` and `Cart._items` are protected and
  only exposed through a validating `@property` or a defensive copy —
  external code cannot corrupt internal state directly.
- **Polymorphism**: `Cart.shipping_total()` calls
  `product.calculate_shipping_cost()` in a single loop over mixed
  subclasses; each object supplies its own behaviour with no `isinstance`
  branching (see the "polymorphism" section of `main.py`'s output).
- **Class methods & static methods**: `Order.from_cart()` (classmethod —
  alternate constructor, respects subclassing) and
  `Order.validate_customer_name()` (staticmethod — pure utility, doesn't
  need instance or class state).
- **Magic/dunder methods**: `__repr__`, `__str__`, `__eq__`, `__hash__`,
  `__lt__` on `Product`; `__len__`, `__bool__` on `Cart`; `__len__`,
  `__repr__` on `Order`.

### 2. Exceptions
- **Custom exceptions**: `EcommerceError` base with
  `InvalidProductDataError`, `InsufficientStockError` (carries structured
  data: product id, requested, available), `OrderValidationError`,
  `PaymentError`.
- **Exception-handling flow**: `Order.place()` reserves items one at a
  time; if one fails partway through, everything already reserved for
  *that* order is released before the error is re-raised, so
  `InventoryManager` never ends up inconsistent. `main.py` shows both the
  generic `except EcommerceError` and specific `except
  InsufficientStockError` styles.

### 3. Context managers
- **Custom (class-based)**: `OrderTransaction` (`__enter__`/`__exit__`)
  wraps order placement with start/commit/rollback logging.
- **contextlib-based**: `log_step()` for lighter-weight before/after
  logging where a full class is unnecessary — shown as a second, simpler
  way to write a context manager for a different use case.

### 4. Design patterns (2+ required — 3 implemented)
| Pattern   | Where              | Why it was chosen |
|-----------|--------------------|--------------------|
| **Factory** | `patterns/factory.py` | Product type arrives as a string from outside the app (a form, a CSV row). One registry avoids duplicating a string→class mapping at every call site and makes adding a product type a one-line change. |
| **Singleton** | `patterns/singleton.py` | Stock counts are shared state that the whole app must agree on; two independent inventories could oversell. `__new__` guarantees one shared instance everywhere. |
| **Strategy** | `patterns/strategy.py` | Discount rules change independently of checkout logic and shouldn't require editing `Order`. Each rule is a class behind one `apply()` interface, swappable per-order at runtime. |

### 5. Refactoring duplicate code
Before `OrderTransaction` existed, every place that placed an order
needed its own `try/except/finally` for logging + rollback. That
duplicated control flow was pulled into a single context manager, so new
call sites get correct behaviour for free just by using `with
OrderTransaction(...):`. Similarly, `ProductFactory` replaced what would
otherwise be a repeated `if product_type == "physical": ... elif ...`
block at every place a product is constructed from raw data.

## Design trade-offs worth noting
- The `Singleton`'s shared state is a deliberate trade-off: it's exactly
  what's needed in production, but it means tests must explicitly reset
  `InventoryManager._instance` between cases (see `test_system.py`) —
  global mutable state is powerful and a little dangerous, which is why
  it's used only for the one thing (stock) that truly must be singular.
- `Order` uses **composition** (`self.discount: DiscountStrategy`) rather
  than subclassing per discount type, so discount and order-placement
  logic can vary independently.
