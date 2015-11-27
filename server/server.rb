require 'socket'

server = TCPServer.new(8080)

client = server.accept
puts 'Connected with client!'
message = client.gets
raise message if message != "NGS HELLO\n"
puts 'Receied HELLO'

message = client.gets
raise message if message.index('CMD PORT') != 0
port = message[(message.rindex(' ') + 1)..-1].to_i
puts 'Received CMD PORT'
client.write("NGS ACK\n\n")
puts 'Wrote ACK'

ip_address = client.remote_address.ip_address
client.close
sleep 0.1
client = TCPSocket.new(ip_address, port)
puts 'Connected with monitor!'
client.write("NGS JOB ADD\nID 6\nTYPE PROCESS\nMETRIC MEMORY\nFREQ 5\nKEEPALIVE FALSE\nPIDFILE /root/working/a.pid\n\n");
puts 'Sent Command'
resp = client.gets
client.gets
resp == "NGS ACK\n" ? puts('Received ACK!') : raise
loop do
  sleep 10
  puts 'Send Keepalive'
  client.write("NGS STILL THERE?\n\n")
  resp = client.gets
  client.gets
  puts resp
end
