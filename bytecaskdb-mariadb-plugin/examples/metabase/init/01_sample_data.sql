-- Analytics database with sample e-commerce data for Metabase to explore.
CREATE DATABASE IF NOT EXISTS analytics;
GRANT ALL PRIVILEGES ON analytics.* TO 'metabase'@'%';

-- Metabase app database (Liquibase migrations run here).
CREATE DATABASE IF NOT EXISTS metabase_app;
GRANT ALL PRIVILEGES ON metabase_app.* TO 'metabase'@'%';

USE analytics;

CREATE TABLE customers (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    email       VARCHAR(150) NOT NULL,
    country     VARCHAR(60)  NOT NULL,
    joined_at   DATE         NOT NULL
);

CREATE TABLE products (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    category    VARCHAR(60)  NOT NULL,
    price       DECIMAL(10,2) NOT NULL
);

CREATE TABLE orders (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    customer_id INT          NOT NULL,
    product_id  INT          NOT NULL,
    quantity    INT          NOT NULL,
    total       DECIMAL(10,2) NOT NULL,
    status      VARCHAR(20)  NOT NULL,
    created_at  DATETIME     NOT NULL
);

INSERT INTO customers (name, email, country, joined_at) VALUES
    ('Alice Martin',   'alice@example.com',   'US', '2024-01-10'),
    ('Bob Silva',      'bob@example.com',     'BR', '2024-02-14'),
    ('Clara Dubois',   'clara@example.com',   'FR', '2024-03-05'),
    ('David Nguyen',   'david@example.com',   'VN', '2024-03-20'),
    ('Elena Rossi',    'elena@example.com',   'IT', '2024-04-01'),
    ('Frank Müller',   'frank@example.com',   'DE', '2024-04-18'),
    ('Grace Okonkwo',  'grace@example.com',   'NG', '2024-05-02'),
    ('Hiro Tanaka',    'hiro@example.com',    'JP', '2024-05-15'),
    ('Isabel Santos',  'isabel@example.com',  'PT', '2024-06-01'),
    ('Jack Williams',  'jack@example.com',    'AU', '2024-06-20');

INSERT INTO products (name, category, price) VALUES
    ('Widget Pro',      'Hardware',  29.99),
    ('Widget Ultra',    'Hardware',  59.99),
    ('Gadget Basic',    'Electronics', 19.99),
    ('Gadget Plus',     'Electronics', 89.99),
    ('Subscription S',  'Software',  9.99),
    ('Subscription M',  'Software',  24.99),
    ('Subscription L',  'Software',  49.99),
    ('Support Pack',    'Services',  99.99),
    ('Consulting Hour', 'Services', 149.99),
    ('Data Bundle',     'Software',  34.99);

INSERT INTO orders (customer_id, product_id, quantity, total, status, created_at) VALUES
    (1, 1, 2,  59.98, 'completed', '2025-01-05 09:00:00'),
    (1, 5, 1,   9.99, 'completed', '2025-01-20 14:30:00'),
    (2, 4, 1,  89.99, 'completed', '2025-02-01 10:00:00'),
    (2, 6, 1,  24.99, 'completed', '2025-02-15 11:00:00'),
    (3, 2, 3, 179.97, 'completed', '2025-02-20 16:00:00'),
    (3, 8, 1,  99.99, 'completed', '2025-03-01 09:30:00'),
    (4, 3, 5,  99.95, 'completed', '2025-03-10 13:00:00'),
    (4, 7, 1,  49.99, 'completed', '2025-03-25 15:00:00'),
    (5, 9, 2, 299.98, 'completed', '2025-04-05 10:00:00'),
    (5, 1, 1,  29.99, 'completed', '2025-04-10 11:30:00'),
    (6, 6, 1,  24.99, 'pending',   '2025-04-20 09:00:00'),
    (6, 4, 2, 179.98, 'completed', '2025-05-01 14:00:00'),
    (7, 5, 1,   9.99, 'completed', '2025-05-10 10:30:00'),
    (7, 2, 1,  59.99, 'refunded',  '2025-05-15 16:00:00'),
    (8, 10,1,  34.99, 'completed', '2025-06-01 09:00:00'),
    (8, 7, 2,  99.98, 'completed', '2025-06-10 11:00:00'),
    (9, 3, 3,  59.97, 'completed', '2025-06-20 13:30:00'),
    (9, 8, 1,  99.99, 'pending',   '2025-07-01 10:00:00'),
    (10,1, 4, 119.96, 'completed', '2025-07-10 14:00:00'),
    (10,9, 1, 149.99, 'completed', '2025-07-20 09:30:00');
