-- gen2 external mirror schema. Separate from gen1 (own DB, e.g. baza23202_gen2).
-- The gateway pushes ready-made rows here; the server never computes anything - it
-- is a passive backup + read-fallback for the app when the gateway is unreachable.
-- Tables mirror the gateway's SQLite 1:1 (only the durable ones: config, nodes,
-- current state, and the solar aggregates - NOT the 2h raw buffer or param_def).

CREATE TABLE IF NOT EXISTS gw_config (
  `key`   VARCHAR(64)  NOT NULL,
  `value` MEDIUMTEXT   NOT NULL,
  PRIMARY KEY (`key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS gw_node (
  node_id        INT NOT NULL,      -- stable logical id (not the RF address)
  address        INT,               -- current RF address, NULL when detached
  node_type      INT NOT NULL,
  name           VARCHAR(128),
  factory_id     VARCHAR(32),
  status         VARCHAR(32),
  provisioned_at BIGINT,
  last_seen      BIGINT,
  room           VARCHAR(128),
  PRIMARY KEY (node_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS gw_node_param (
  node_id   INT NOT NULL,
  param_key VARCHAR(64) NOT NULL,
  value_num DOUBLE,
  ts        BIGINT,
  PRIMARY KEY (node_id, param_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS gw_solar_hourly (
  node_id    INT NOT NULL,
  bucket     BIGINT NOT NULL,
  hour_yield DOUBLE NOT NULL,
  hour_pump  INT NOT NULL,
  day_yield  DOUBLE NOT NULL,
  day_pump   INT NOT NULL,
  PRIMARY KEY (node_id, bucket)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS gw_solar_daily (
  node_id     INT NOT NULL,
  bucket      BIGINT NOT NULL,
  day_yield   DOUBLE NOT NULL,
  month_yield DOUBLE NOT NULL,
  month_pump  INT NOT NULL,
  PRIMARY KEY (node_id, bucket)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS gw_solar_monthly (
  node_id     INT NOT NULL,
  bucket      BIGINT NOT NULL,
  month_yield DOUBLE NOT NULL,
  year_yield  DOUBLE NOT NULL,
  year_pump   INT NOT NULL,
  PRIMARY KEY (node_id, bucket)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
