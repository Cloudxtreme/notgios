require 'socket'
require 'byebug'

Thread.abort_on_exception = true

class Object
  def exists?
    true
  end
end

class NilClass
  def exists?
    false
  end
end

class TCPSocket

  def get_message(timeout = 20)
    if IO.select([self], nil, nil, timeout)
      message, tmp = String.new, gets
      until tmp == "\n"
        message += tmp
        tmp = gets
      end
      message.split("\n")
    else
      Array.new
    end
  end

  def send_message(msg)
    case msg
    when Array
      sendable = msg.take_while { |elem| elem }
      write(sendable.join("\n") + "\n\n")
    when String
      write(msg + "\n\n")
    end
  end

end

incoming, outgoing = Array.new, Array.new
server = TCPServer.new(8080)

client = server.accept
puts 'Connected with client!'
messages = client.get_message
raise 'Bad handshake' unless messages[0] == 'NGS HELLO'
puts 'Receied HELLO'

raise 'Bad handshake' if messages[1].index('CMD PORT') != 0
port = messages[1][(messages[1].rindex(' ') + 1)..-1].to_i
puts 'Received CMD PORT'
client.send_message('NGS ACK')
puts 'Wrote ACK'

ip_address = client.remote_address.ip_address
client.close
sleep 1
client = TCPSocket.new(ip_address, port)
puts 'Connected with monitor!'
client.send_message([
  'NGS JOB ADD',
  'ID 6',
  'TYPE PROCESS',
  'METRIC MEMORY',
  'FREQ 5',
  'KEEPALIVE FALSE',
  'PIDFILE /root/working/a.pid'
])
puts 'Sent Command'
client.get_message[0] == 'NGS ACK' ? puts('Received ACK!') : raise

Thread.new do
  keepalive_timer = Time.now
  loop do
    if Time.now - keepalive_timer >= 10
      client.send_message('NGS STILL THERE?')
      loop do
        message = client.get_message(10)
        if message[0].exists? && message[0] == 'NGS STILL HERE!'
          keepalive_timer = Time.now
          break
        elsif message[0].exists? && message[0] != 'NGS STILL HERE!'
          incoming.push(message)
        else
          raise 'Client failed to send keepalive'
        end
      end
    else
      client.send_message(outgoing) unless outgoing.empty?
      incoming.concat(client.get_message((10 - (Time.now - keepalive_timer)).abs))
    end
  end
end

loop do
  elem = incoming.shift
  puts elem if elem.exists?
end
