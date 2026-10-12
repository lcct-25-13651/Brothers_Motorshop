#include "httplib.h"
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>

class Part {
public:
    int id;
    std::string name;
    double price;
    int stock;

    Part(int id, std::string name, double price, int stock)
        : id(id), name(name), price(price), stock(stock) {}

    std::string toCSV() const {
        return std::to_string(id) + "," + name + "," + std::to_string(price) + "," + std::to_string(stock);
    }
};

class Service {
public:
    int id;
    std::string description;
    double laborCost;

    Service(int id, std::string description, double laborCost)
        : id(id), description(description), laborCost(laborCost) {}

    std::string toCSV() const {
        return std::to_string(id) + "," + description + "," + std::to_string(laborCost);
    }
};

class InventorySystem {
public:
    std::vector<Part> parts;
    std::vector<Service> services;

    InventorySystem() { loadData(); }

    void addPart(const Part& part) {
        parts.push_back(part);
        saveData();
    }

    void addService(const Service& service) {
        services.push_back(service);
        saveData();
    }

    void saveData() const {
        std::ofstream pOut("parts.csv");
        for (const auto& part : parts) pOut << part.toCSV() << "\n";
        pOut.close();

        std::ofstream sOut("services.csv");
        for (const auto& service : services) sOut << service.toCSV() << "\n";
        sOut.close();
    }

    void loadData() {
        parts.clear();
        services.clear();

        std::ifstream pIn("parts.csv");
        if (pIn.is_open()) {
            std::string line;
            while (std::getline(pIn, line)) {
                if (line.empty()) continue;
                std::stringstream ss(line);
                std::string sId, name, sPrice, sStock;
                std::getline(ss, sId, ',');
                std::getline(ss, name, ',');
                std::getline(ss, sPrice, ',');
                std::getline(ss, sStock, ',');
                if (!sId.empty()) {
                    parts.push_back(Part(std::stoi(sId), name, std::stod(sPrice), std::stoi(sStock)));
                }
            }
            pIn.close();
        }

        std::ifstream sIn("services.csv");
        if (sIn.is_open()) {
            std::string line;
            while (std::getline(sIn, line)) {
                if (line.empty()) continue;
                std::stringstream ss(line);
                std::string sId, desc, sCost;
                std::getline(ss, sId, ',');
                std::getline(ss, desc, ',');
                std::getline(ss, sCost, ',');
                if (!sId.empty()) {
                    services.push_back(Service(std::stoi(sId), desc, std::stod(sCost)));
                }
            }
            sIn.close();
        }

        if (parts.empty() && services.empty()) {
            parts.push_back(Part(101, "Engine Oil 1L", 450.00, 20));
            parts.push_back(Part(102, "Brake Pad Set", 850.50, 12));
            services.push_back(Service(201, "Change Oil Service", 250.00));
            saveData();
        }
    }
};

std::string formatPrice(double price) {
    std::stringstream stream;
    stream << std::fixed << std::setprecision(2) << price;
    return stream.str();
}

int main() {
    httplib::Server svr;
    InventorySystem system;

    svr.Get("/", [&system](const httplib::Request&, httplib::Response& res) {
        std::string html = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Brothers Motorshop Web App</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 30px; background-color: #f4f6f9; }
        h1 { color: #1a365d; }
        .card { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 20px; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th, td { border: 1px solid #ddd; padding: 10px; text-align: left; }
        th { background-color: #2b6cb0; color: white; }
        input, button { padding: 8px; margin: 5px 0; }
        button { background-color: #2b6cb0; color: white; border: none; border-radius: 4px; cursor: pointer; }
        button:hover { background-color: #2c5282; }
    </style>
</head>
<body>
    <h1>Brothers Motorshop Management System</h1>
    <div class="card">
        <h2>Parts Inventory</h2>
        <table>
            <tr><th>ID</th><th>Part Name</th><th>Price (PHP)</th><th>Stock</th></tr>
)html";

        for (const auto& part : system.parts) {
            html += "<tr><td>" + std::to_string(part.id) + "</td><td>" + part.name + "</td><td>"
                 + formatPrice(part.price) + "</td><td>" + std::to_string(part.stock) + "</td></tr>\n";
        }

        html += R"html(
        </table>
        <h3>Add New Part</h3>
        <form action="/add-part" method="POST">
            <input type="number" name="id" placeholder="Part ID" required>
            <input type="text" name="name" placeholder="Part Name" required>
            <input type="number" step="0.01" name="price" placeholder="Price" required>
            <input type="number" name="stock" placeholder="Stock" required><br>
            <button type="submit">Add Part</button>
        </form>
    </div>

    <div class="card">
        <h2>Services Offered</h2>
        <table>
            <tr><th>ID</th><th>Service Description</th><th>Labor Cost (PHP)</th></tr>
)html";

        for (const auto& service : system.services) {
            html += "<tr><td>" + std::to_string(service.id) + "</td><td>" + service.description + "</td><td>"
                 + formatPrice(service.laborCost) + "</td></tr>\n";
        }

        html += R"html(
        </table>
    </div>
</body>
</html>
)html";

        res.set_content(html, "text/html; charset=utf-8");
    });

    svr.Post("/add-part", [&system](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("id") && req.has_param("name") && req.has_param("price") && req.has_param("stock")) {
            int id = std::stoi(req.get_param_value("id"));
            std::string name = req.get_param_value("name");
            double price = std::stod(req.get_param_value("price"));
            int stock = std::stoi(req.get_param_value("stock"));
            system.addPart(Part(id, name, price, stock));
        }
        res.set_redirect("/");
    });

    std::cout << "Starting C++ Web Server at http://localhost:8080\n";
    svr.listen("0.0.0.0", 8080);
}