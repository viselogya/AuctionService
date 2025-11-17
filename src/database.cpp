#include "database.h"

#include <stdexcept>
#include <vector>

#include <pqxx/pqxx>

namespace {

nlohmann::json row_to_json(const pqxx::row& row) {
    nlohmann::json lot;
    lot["id"] = row["id"].as<int>();
    lot["name"] = row["name"].as<std::string>();
    if (row["description"].is_null()) {
        lot["description"] = nullptr;
    } else {
        lot["description"] = row["description"].as<std::string>();
    }
    lot["start_price"] = row["start_price"].as<double>();
    if (row["current_price"].is_null()) {
        lot["current_price"] = nullptr;
    } else {
        lot["current_price"] = row["current_price"].as<double>();
    }
    if (row["owner_id"].is_null()) {
        lot["owner_id"] = nullptr;
    } else {
        lot["owner_id"] = row["owner_id"].as<std::string>();
    }
    lot["created_at"] = row["created_at"].as<std::string>();
    lot["auction_end_date"] = row["auction_end_date"].as<std::string>();
    return lot;
}

} // namespace

Database::Database(std::string connection_uri)
    : connection_uri_(std::move(connection_uri)) {
    if (connection_uri_.empty()) {
        throw std::invalid_argument("Database connection string must not be empty");
    }
}

void Database::ensure_schema() {
    pqxx::connection conn(connection_uri_);
    pqxx::work txn(conn);
    txn.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS lots (
            id SERIAL PRIMARY KEY,
            name VARCHAR(255) NOT NULL,
            description TEXT,
            start_price DECIMAL(12, 2) NOT NULL,
            current_price DECIMAL(12, 2),
            owner_id VARCHAR(255),
            created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
            auction_end_date TIMESTAMP WITH TIME ZONE NOT NULL
        )
    )SQL");
    txn.commit();
}

nlohmann::json Database::get_all_lots() {
    pqxx::connection conn(connection_uri_);
    pqxx::work txn(conn);

    nlohmann::json items = nlohmann::json::array();
    auto result = txn.exec("SELECT * FROM lots ORDER BY id");
    for (const auto& row : result) {
        items.push_back(row_to_json(row));
    }
    txn.commit();

    return items;
}

std::optional<nlohmann::json> Database::get_lot_by_id(int lot_id) {
    pqxx::connection conn(connection_uri_);
    pqxx::work txn(conn);

    auto result = txn.exec_params("SELECT * FROM lots WHERE id = $1", lot_id);
    txn.commit();

    if (result.empty()) {
        return std::nullopt;
    }
    return row_to_json(result[0]);
}

nlohmann::json Database::create_lot(const LotCreateParams& params) {
    pqxx::connection conn(connection_uri_);
    pqxx::work txn(conn);

    auto result = txn.exec_params(
        R"SQL(
            INSERT INTO lots (name, description, start_price, current_price, owner_id, auction_end_date)
            VALUES ($1, $2, $3, $3, $4, COALESCE($5::timestamptz, CURRENT_TIMESTAMP + INTERVAL '7 days'))
            RETURNING *
        )SQL",
        params.name,
        params.description ? params.description->c_str() : pqxx::null(),
        params.start_price,
        params.owner_id ? params.owner_id->c_str() : pqxx::null(),
        params.auction_end_date ? params.auction_end_date->c_str() : pqxx::null()
    );

    txn.commit();

    if (result.empty()) {
        throw std::runtime_error("Failed to insert lot");
    }

    return row_to_json(result[0]);
}

std::optional<nlohmann::json> Database::update_lot(int lot_id, const LotUpdateParams& params) {
    if (!params.name_present && !params.description_present && !params.owner_id_present &&
        !params.auction_end_date_present && !params.current_price_present) {
        return get_lot_by_id(lot_id);
    }

    pqxx::connection conn(connection_uri_);
    pqxx::work txn(conn);

    std::vector<std::string> updates;

    if (params.name_present) {
        if (params.name) {
            updates.emplace_back("name = " + txn.quote(*params.name));
        } else {
            updates.emplace_back("name = NULL");
        }
    }
    if (params.description_present) {
        if (params.description) {
            updates.emplace_back("description = " + txn.quote(*params.description));
        } else {
            updates.emplace_back("description = NULL");
        }
    }
    if (params.owner_id_present) {
        if (params.owner_id) {
            updates.emplace_back("owner_id = " + txn.quote(*params.owner_id));
        } else {
            updates.emplace_back("owner_id = NULL");
        }
    }
    if (params.auction_end_date_present) {
        if (params.auction_end_date) {
            updates.emplace_back("auction_end_date = " + txn.quote(*params.auction_end_date) + "::timestamptz");
        } else {
            updates.emplace_back("auction_end_date = NULL");
        }
    }
    if (params.current_price_present) {
        if (params.current_price) {
            updates.emplace_back("current_price = " + pqxx::to_string(*params.current_price));
        } else {
            updates.emplace_back("current_price = NULL");
        }
    }

    if (updates.empty()) {
        txn.abort();
        return get_lot_by_id(lot_id);
    }

    std::string sql = "UPDATE lots SET ";
    for (std::size_t i = 0; i < updates.size(); ++i) {
        sql += updates[i];
        if (i + 1 < updates.size()) {
            sql += ", ";
        }
    }
    sql += " WHERE id = " + txn.quote(lot_id) + " RETURNING *";

    auto result = txn.exec(sql);
    txn.commit();

    if (result.empty()) {
        return std::nullopt;
    }

    return row_to_json(result[0]);
}

bool Database::delete_lot(int lot_id) {
    pqxx::connection conn(connection_uri_);
    pqxx::work txn(conn);
    auto result = txn.exec_params("DELETE FROM lots WHERE id = $1", lot_id);
    auto affected = result.affected_rows();
    txn.commit();
    return affected > 0;
}

std::optional<nlohmann::json> Database::place_bid(int lot_id, double bid_amount, std::string& error_reason) {
    pqxx::connection conn(connection_uri_);
    pqxx::work txn(conn);

    auto select_result = txn.exec_params("SELECT * FROM lots WHERE id = $1 FOR UPDATE", lot_id);
    if (select_result.empty()) {
        error_reason = "Lot not found";
        txn.commit();
        return std::nullopt;
    }

    const auto& row = select_result[0];
    double baseline_price = row["current_price"].is_null() ? row["start_price"].as<double>() : row["current_price"].as<double>();
    if (bid_amount <= baseline_price) {
        error_reason = "Bid must be greater than current price";
        txn.commit();
        return std::nullopt;
    }

    auto auction_open_result = txn.exec_params(
        "SELECT $1::timestamptz > CURRENT_TIMESTAMP",
        row["auction_end_date"].as<std::string>()
    );
    bool auction_open = !auction_open_result.empty() && auction_open_result[0][0].as<bool>();

    if (!auction_open) {
        error_reason = "Auction has ended";
        txn.commit();
        return std::nullopt;
    }

    auto update_result = txn.exec_params(
        R"SQL(
            UPDATE lots
            SET current_price = $2
            WHERE id = $1
            RETURNING *
        )SQL",
        lot_id,
        bid_amount
    );

    if (update_result.empty()) {
        error_reason = "Failed to update bid";
        txn.commit();
        return std::nullopt;
    }

    txn.commit();
    return row_to_json(update_result[0]);
}

void Database::check_connection() {
    pqxx::connection conn(connection_uri_);
    if (!conn.is_open()) {
        throw std::runtime_error("Database connection is not open");
    }

    pqxx::work txn(conn);
    auto result = txn.exec("SELECT 1");
    txn.commit();

    if (result.empty()) {
        throw std::runtime_error("Database connectivity check failed");
    }
}

