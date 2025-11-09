#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "database.h"
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;

namespace {

const std::string kServiceName = "AuctionService";

std::string require_env(const char* name) {
    const char* value = std::getenv(name);
    if (!value || std::string(value).empty()) {
        throw std::runtime_error(std::string("Missing environment variable: ") + name);
    }
    return value;
}

std::optional<int> parse_path_id(const httplib::Request& req) {
    if (req.matches.size() < 2) {
        return std::nullopt;
    }
    try {
        return std::stoi(req.matches[1]);
    } catch (...) {
        return std::nullopt;
    }
}

struct TokenValidationResult {
    bool allowed{false};
    int http_status{403};
    std::string message;
};

TokenValidationResult check_token(const std::string& payment_service_url,
                                  const std::string& method_name,
                                  const std::string& token) {
    if (payment_service_url.empty()) {
        return {false, 500, "Payment service URL is not configured"};
    }

    httplib::Client client(payment_service_url.c_str());
    client.set_keep_alive(true);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);

    json payload{
        {"token", token},
        {"serviceName", kServiceName},
        {"methodName", method_name}
    };

    try {
        auto response = client.Post("/token/check", payload.dump(), "application/json");
        if (!response) {
            return {false, 502, "Payment service unavailable"};
        }
        if (response->status >= 500) {
            return {false, 502, "Payment service error"};
        }
        if (response->status == 401) {
            return {false, 401, "Invalid token"};
        }
        if (response->status >= 400) {
            return {false, 403, "Token validation failed"};
        }
        auto body = json::parse(response->body, nullptr, true, true);
        bool allowed = body.value("allowed", false);
        if (!allowed) {
            return {false, 403, "Access denied"};
        }
        return {true, 200, "Allowed"};
    } catch (const std::exception& ex) {
        return {false, 502, std::string("Payment service call failed: ") + ex.what()};
    }
}

std::optional<std::string> extract_bearer_token(const httplib::Request& req, std::string& error_message) {
    auto header = req.get_header_value("Authorization");
    if (header.empty()) {
        error_message = "Authorization header is required";
        return std::nullopt;
    }
    const std::string prefix = "Bearer ";
    if (header.rfind(prefix, 0) != 0) {
        error_message = "Authorization header must use Bearer scheme";
        return std::nullopt;
    }
    std::string token = header.substr(prefix.size());
    if (token.empty()) {
        error_message = "Bearer token must not be empty";
        return std::nullopt;
    }
    return token;
}

int register_service(const std::string& registry_service_url,
                     const std::string& service_address,
                     const std::vector<std::string>& methods) {
    httplib::Client client(registry_service_url.c_str());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);

    json service_payload{
        {"ServiceName", kServiceName},
        {"address", service_address}
    };

    auto service_response = client.Post("/server", service_payload.dump(), "application/json");
    if (!service_response) {
        throw std::runtime_error("Failed to reach registry service");
    }
    if (service_response->status >= 400) {
        throw std::runtime_error("Registry service rejected registration: " + std::to_string(service_response->status));
    }

    auto body = json::parse(service_response->body, nullptr, true, true);
    int service_id = -1;
    if (body.contains("id") && body["id"].is_number()) {
        service_id = body["id"].get<int>();
    } else if (body.contains("ID") && body["ID"].is_number()) {
        service_id = body["ID"].get<int>();
    } else if (body.contains("ServiceModelID") && body["ServiceModelID"].is_number()) {
        service_id = body["ServiceModelID"].get<int>();
    } else if (body.contains("data") && body["data"].is_object()) {
        const auto& data = body["data"];
        if (data.contains("id") && data["id"].is_number()) {
            service_id = data["id"].get<int>();
        }
    }

    if (service_id <= 0) {
        throw std::runtime_error("Unable to determine service id from registry response");
    }

    for (const auto& method : methods) {
        json method_payload{
            {"MethodName", method},
            {"IsPrivate", false},
            {"ServiceModelID", service_id}
        };
        auto method_response = client.Post("/method", method_payload.dump(), "application/json");
        if (!method_response || method_response->status >= 400) {
            throw std::runtime_error("Failed to register method '" + method + "'");
        }
    }

    return service_id;
}

json make_error(const std::string& message) {
    return json{
        {"error", message}
    };
}

void send_json(httplib::Response& res, int status, const json& payload) {
    res.status = status;
    res.set_content(payload.dump(), "application/json");
}

} // namespace

int main() {
    try {
        const std::string database_url = require_env("DATABASE_URL");
        const std::string registry_service_url = require_env("REGISTRY_SERVICE_URL");
        const std::string payment_service_url = require_env("PAYMENT_SERVICE_URL");
        const std::string service_port_str = require_env("SERVICE_PORT");

        int service_port = 0;
        try {
            service_port = std::stoi(service_port_str);
        } catch (...) {
            throw std::runtime_error("SERVICE_PORT must be a valid integer");
        }
        if (service_port <= 0 || service_port > 65535) {
            throw std::runtime_error("SERVICE_PORT must be between 1 and 65535");
        }

        const std::string service_address = "http://auction-service:" + std::to_string(service_port);

        Database database(database_url);
        database.ensure_schema();

        std::vector<std::string> payable_methods = {"PlaceBid", "CreateLot", "UpdateLot", "DeleteLot"};
        try {
            register_service(registry_service_url, service_address, payable_methods);
            std::cout << "Successfully registered service with registry" << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Service registration failed: " << ex.what() << std::endl;
        }

        httplib::Server server;

        server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            send_json(res, 200, json{{"status", "ok"}});
        });

        server.Get("/lots", [&database](const httplib::Request&, httplib::Response& res) {
            try {
                auto lots = database.get_all_lots();
                send_json(res, 200, lots);
            } catch (const std::exception& ex) {
                send_json(res, 500, make_error(ex.what()));
            }
        });

        server.Get(R"(/lots/(\d+))", [&database](const httplib::Request& req, httplib::Response& res) {
            auto lot_id = parse_path_id(req);
            if (!lot_id) {
                send_json(res, 400, make_error("Invalid lot id"));
                return;
            }

            try {
                auto lot = database.get_lot_by_id(*lot_id);
                if (!lot) {
                    send_json(res, 404, make_error("Lot not found"));
                    return;
                }
                send_json(res, 200, *lot);
            } catch (const std::exception& ex) {
                send_json(res, 500, make_error(ex.what()));
            }
        });

        auto require_paid_access = [&payment_service_url](const httplib::Request& req,
                                                          httplib::Response& res,
                                                          const std::string& method_name) -> std::optional<std::string> {
            std::string token_error;
            auto token = extract_bearer_token(req, token_error);
            if (!token) {
                send_json(res, 401, make_error(token_error));
                return std::nullopt;
            }

            auto validation = check_token(payment_service_url, method_name, *token);
            if (!validation.allowed) {
                send_json(res, validation.http_status, make_error(validation.message));
                return std::nullopt;
            }
            return token;
        };

        server.Post("/lots", [&database, &require_paid_access](const httplib::Request& req, httplib::Response& res) {
            if (!require_paid_access(req, res, "CreateLot")) {
                return;
            }

            try {
                auto payload = json::parse(req.body);
                if (!payload.contains("name") || !payload.contains("start_price") || !payload.contains("auction_end_date")) {
                    send_json(res, 400, make_error("Missing required fields: name, start_price, auction_end_date"));
                    return;
                }

                LotCreateParams params{
                    payload["name"].get<std::string>(),
                    payload.contains("description") && !payload["description"].is_null()
                        ? std::optional<std::string>(payload["description"].get<std::string>())
                        : std::nullopt,
                    payload["start_price"].get<double>(),
                    payload.contains("owner_id") && !payload["owner_id"].is_null()
                        ? std::optional<std::string>(payload["owner_id"].get<std::string>())
                        : std::nullopt,
                    payload["auction_end_date"].get<std::string>()
                };

                auto created = database.create_lot(params);
                send_json(res, 201, created);
            } catch (const json::parse_error&) {
                send_json(res, 400, make_error("Invalid JSON payload"));
            } catch (const json::type_error& ex) {
                send_json(res, 400, make_error(std::string("Invalid field type: ") + ex.what()));
            } catch (const std::exception& ex) {
                send_json(res, 500, make_error(ex.what()));
            }
        });

        server.Put(R"(/lots/(\d+))", [&database, &require_paid_access](const httplib::Request& req, httplib::Response& res) {
            if (!require_paid_access(req, res, "UpdateLot")) {
                return;
            }

            auto lot_id = parse_path_id(req);
            if (!lot_id) {
                send_json(res, 400, make_error("Invalid lot id"));
                return;
            }

            try {
                auto payload = json::parse(req.body);

                LotUpdateParams params{};
                if (payload.contains("name")) {
                    params.name_present = true;
                    if (payload["name"].is_null()) {
                        params.name = std::nullopt;
                    } else {
                        params.name = payload["name"].get<std::string>();
                    }
                }
                if (payload.contains("description")) {
                    params.description_present = true;
                    if (payload["description"].is_null()) {
                        params.description = std::nullopt;
                    } else {
                        params.description = payload["description"].get<std::string>();
                    }
                }
                if (payload.contains("owner_id")) {
                    params.owner_id_present = true;
                    if (payload["owner_id"].is_null()) {
                        params.owner_id = std::nullopt;
                    } else {
                        params.owner_id = payload["owner_id"].get<std::string>();
                    }
                }

                auto updated = database.update_lot(*lot_id, params);
                if (!updated) {
                    send_json(res, 404, make_error("Lot not found"));
                    return;
                }
                send_json(res, 200, *updated);
            } catch (const json::parse_error&) {
                send_json(res, 400, make_error("Invalid JSON payload"));
            } catch (const json::type_error& ex) {
                send_json(res, 400, make_error(std::string("Invalid field type: ") + ex.what()));
            } catch (const std::exception& ex) {
                send_json(res, 500, make_error(ex.what()));
            }
        });

        server.Delete(R"(/lots/(\d+))", [&database, &require_paid_access](const httplib::Request& req, httplib::Response& res) {
            if (!require_paid_access(req, res, "DeleteLot")) {
                return;
            }

            auto lot_id = parse_path_id(req);
            if (!lot_id) {
                send_json(res, 400, make_error("Invalid lot id"));
                return;
            }

            try {
                bool deleted = database.delete_lot(*lot_id);
                if (!deleted) {
                    send_json(res, 404, make_error("Lot not found"));
                    return;
                }
                res.status = 204;
            } catch (const std::exception& ex) {
                send_json(res, 500, make_error(ex.what()));
            }
        });

        server.Post(R"(/lots/(\d+)/bid)", [&database, &require_paid_access](const httplib::Request& req, httplib::Response& res) {
            if (!require_paid_access(req, res, "PlaceBid")) {
                return;
            }

            auto lot_id = parse_path_id(req);
            if (!lot_id) {
                send_json(res, 400, make_error("Invalid lot id"));
                return;
            }

            try {
                auto payload = json::parse(req.body);
                if (!payload.contains("bid_amount")) {
                    send_json(res, 400, make_error("Missing field: bid_amount"));
                    return;
                }
                double bid_amount = payload["bid_amount"].get<double>();

                std::string error_reason;
                auto updated = database.place_bid(*lot_id, bid_amount, error_reason);
                if (!updated) {
                    if (error_reason == "Lot not found") {
                        send_json(res, 404, make_error(error_reason));
                    } else if (error_reason == "Bid must be greater than current price") {
                        send_json(res, 400, make_error(error_reason));
                    } else if (error_reason == "Auction has ended") {
                        send_json(res, 409, make_error(error_reason));
                    } else {
                        send_json(res, 400, make_error(error_reason));
                    }
                    return;
                }

                send_json(res, 200, *updated);
            } catch (const json::parse_error&) {
                send_json(res, 400, make_error("Invalid JSON payload"));
            } catch (const json::type_error& ex) {
                send_json(res, 400, make_error(std::string("Invalid field type: ") + ex.what()));
            } catch (const std::exception& ex) {
                send_json(res, 500, make_error(ex.what()));
            }
        });

        std::cout << "AuctionService listening on port " << service_port << std::endl;
        if (!server.listen("0.0.0.0", service_port)) {
            throw std::runtime_error("Failed to start HTTP server");
        }
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

