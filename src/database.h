#pragma once

#include <optional>
#include <string>

#include "json.hpp"

struct LotCreateParams {
    std::string name;
    std::optional<std::string> description;
    double start_price;
    std::optional<std::string> owner_id;
    std::string auction_end_date;
};

struct LotUpdateParams {
    bool name_present{false};
    std::optional<std::string> name;
    bool description_present{false};
    std::optional<std::string> description;
    bool owner_id_present{false};
    std::optional<std::string> owner_id;
};

class Database {
public:
    explicit Database(std::string connection_uri);

    void ensure_schema();

    nlohmann::json get_all_lots();
    std::optional<nlohmann::json> get_lot_by_id(int lot_id);
    nlohmann::json create_lot(const LotCreateParams& params);
    std::optional<nlohmann::json> update_lot(int lot_id, const LotUpdateParams& params);
    bool delete_lot(int lot_id);
    std::optional<nlohmann::json> place_bid(int lot_id, double bid_amount, std::string& error_reason);

private:
    std::string connection_uri_;
};

