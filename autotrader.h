// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#ifndef CPPREADY_TRADER_GO_AUTOTRADER_H
#define CPPREADY_TRADER_GO_AUTOTRADER_H

#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <thread>
#include <boost/asio/io_context.hpp>
#include <iostream>
#include <vector>

#include <ready_trader_go/baseautotrader.h>
#include <ready_trader_go/types.h>

#include <deque>
#include <chrono>
#include <limits>

typedef long long ll;
using namespace std;
using namespace ReadyTraderGo;
const int SPEED = 1;
class FrequencyLimiter {
public:

    int check_remaining() {
        const std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::high_resolution_clock::now();
        auto window_start = now - interval;

        while(events.size() > 0 && events.front() < window_start)
            events.pop_front();

        return limit - events.size();
    }

    bool check_and_add() {
        const std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::high_resolution_clock::now();
        auto window_start = now - interval;

        while(events.size() > 0 && events.front() < window_start)
            events.pop_front();
        
        if(events.size() < limit) {
            events.push_back(now);
            return true;
        }

        return false;
    }

private:
    std::deque<std::chrono::steady_clock::time_point> events;
    std::chrono::duration<double> interval = chrono::microseconds( 1020000 / SPEED);
    int limit = 50;
    std::mutex mtx; 
};

struct order{
    int id = 0, size = 0, price = 0, cancelled = 0;
};

class orders {
private:
    // If order 0 is 0, then order 1 is 0
    order O[2];

    void place(){
        if(O[0].id == 0){
            swap(O[0], O[1]);
        }
    }

public:
    int totsz(){
        return O[0].size + O[1].size;
    }

    bool contains(int a) {
        return (O[0].id == a) || (O[1].id == a);
    }

    bool contains_price(int a) {
        return (O[0].price == a) || (O[1].price == a);
    }

    int cnt(){
        return (O[0].id!=0) + (O[1].id!=0);
    }

    void update(int a,int b){
        int i = (O[1].id == a);
        //assert(O[i].id == a);
        O[i].size = b;
        if(b == 0) O[i] = order();
        place();
    }

    vector<int> to_cancel_ne(int price){
        //assert(price != 0);

        vector<int> ret;
        if(O[0].price != price && O[0].id != 0 && O[0].cancelled == 0){
            ret.push_back(O[0].id);
        }

        if(O[1].price != price && O[1].id != 0 && O[1].cancelled == 0){
            ret.push_back(O[1].id);
        }

        return ret;
    }

    void cancel(int id) {
        //assert(contains(id));
        //assert(O[0].id != O[1].id);
        int i = (O[1].id == id);
        O[i].cancelled = 1;
    }

    void insert(int id, int price, int size){
        //assert(cnt() < 2);
        //assert(id != 0);
        int i = cnt();
        O[i].id = id;
        O[i].price = price;
        O[i].size = size;
        O[i].cancelled = 0;
    }
    
};

class AutoTrader : public ReadyTraderGo::BaseAutoTrader
{
    void try_hedge();
    void cancellation_loop();
    std::thread cloop;
    std::mutex mtx; 
    int future_l = 0;
public:
    explicit AutoTrader(boost::asio::io_context& context);

    // Called when the execution connection is lost.
    void DisconnectHandler() override;

    // Called when the matching engine detects an error.
    // If the error pertains to a particular order, then the client_order_id
    // will identify that order, otherwise the client_order_id will be zero.
    void ErrorMessageHandler(unsigned long clientOrderId,
                             const std::string& errorMessage) override;

    // Called when one of your hedge orders is filled, partially or fully.
    //
    // The price is the average price at which the order was (partially) filled,
    // which may be better than the order's limit price. The volume is
    // the number of lots filled at that price.
    //
    // If the order was unsuccessful, both the price and volume will be zero.
    void HedgeFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called periodically to report the status of an order book.
    // The sequence number can be used to detect missed or out-of-order
    // messages. The five best available ask (i.e. sell) and bid (i.e. buy)
    // prices are reported along with the volume available at each of those
    // price levels.
    void OrderBookMessageHandler(ReadyTraderGo::Instrument instrument,
                                 unsigned long sequenceNumber,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askVolumes,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidVolumes) override;

    // Called when one of your orders is filled, partially or fully.
    void OrderFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called when the status of one of your orders changes.
    // The fill volume is the number of lots already traded, remaining volume
    // is the number of lots yet to be traded and fees is the total fees paid
    // or received for this order.
    // Remaining volume will be set to zero if the order is cancelled.
    void OrderStatusMessageHandler(unsigned long clientOrderId,
                                   unsigned long fillVolume,
                                   unsigned long remainingVolume,
                                   signed long fees) override;

    // Called periodically when there is trading activity on the market.
    // The five best ask (i.e. sell) and bid (i.e. buy) prices at which there
    // has been trading activity are reported along with the aggregated volume
    // traded at each of those price levels.
    // If there are less than five prices on a side, then zeros will appear at
    // the end of both the prices and volumes arrays.
    void TradeTicksMessageHandler(ReadyTraderGo::Instrument instrument,
                                  unsigned long sequenceNumber,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askVolumes,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidVolumes) override;

private:
    unsigned long mNextMessageId = 1;
    unsigned long mAskId = 0;
    unsigned long mAskPrice = 0;
    unsigned long mBidId = 0;
    unsigned long mBidPrice = 0;
    unsigned long newAskPrice = 0;
    unsigned long newBidPrice = 0;
    bool start = 0;
    FrequencyLimiter limiter;
    int AC = 0;
    int BC = 0;
    int mPosition = 0;
    int etfA = 0, etfB = 0;
    int theo_fut = -1, theo_etf = -1, theo = -1;
    int target_hedge = 0;
    int current_hedge = 0;
    int cc = 0;

    orders asks, bids;
    
    unordered_set<int> hedgeS,hedgeB;
    
    std::chrono::steady_clock::time_point last_future_info = std::chrono::high_resolution_clock::now();
    
    void test_place_order();
    void test_get_info();
    void cancel_and_place();
    void new_fut_price(int maxask, int askvol, int minbid, int bidvol);
    bool rate_limited_cancel(unsigned long clientOrderId);
    bool rate_limited_hedge(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume);
    bool rate_limited_insert(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume, Lifespan lifespan);
};

#endif //CPPREADY_TRADER_GO_AUTOTRADER_H
