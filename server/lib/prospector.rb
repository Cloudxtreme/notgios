module Notgios
  class Prospector

    def initialize(twilio_number, twilio_sid, twilio_auth_token)
      @sid = twilio_sid
      @auth_token = twilio_auth_token
      @client = Twilio::REST::Client.new(account_sid, auth_token)
      @twilio_number = twilio_number
    end

    def prospect
      Thread.new do
        Helpers.with_nodis do |nodis|
          loop do
            nodis.get_alarms do |alarm|
              metrics = nodis.get_recent_metrics(alarm['task'])
              if alarm['type'] == 'simple'
                previous = alarm['previous']
                if previous.exists?
                  index = metrics.find_index { |metric| metric['timestamp'] == previous }
                  subset = metrics[0..index]
                else
                  subset = [metrics[0]]
                end
                subset.each do |report|
                  key = nil
                  report.keys.each do |k|
                    if k != 'timestamp'
                      key = k
                      break
                    end
                  end
                  if report[key].to_i > alarm['threshold']
                    @client.messages.create(
                      from: @twilio_number,
                      to: alarm['number'],
                      body: alarm['message']
                    )
                  end
                end
                alarm['previous'] = metrics[0]['timestamp']
              else
                window = alarm['window']
                start = Time.now.to_i
                index = metrics.find_index { |metric| start - alarm['timestamp'].to_i > window }
                subset = metrics[0...index]
                max, min = subset.max, subset.min
                if max - min > alarm['threshold']
                  @client.message.create(
                    from: @twilio_number,
                    fo: alarm['number'],
                    body: alarm['message']
                  )
                end
              end
            end
            sleep 10
          end
        end
      end
    end

  end
end
