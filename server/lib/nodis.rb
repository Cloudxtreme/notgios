module Notgios

  class Nodis < Redis

    ResourceExistsError = Class.new(StandardError)
    NoSuchResourceError = Class.new(StandardError)
    InvalidJobError = Class.new(StandardError)
    UnsupportedJobError = Class.new(StandardError)
    WrongUserError = Class.new(StandardError)

    # User Commands

    def get_users
      smembers('notgios.users')
    end

    def add_user(username, pass)
      raise ResourceExistsError, "User #{username} already exists." if exists("notgios.users.#{username}")
      salt = SecureRandom.hex(16)
      crypted = Digest::SHA256.hexdigest("#{pass}|#{salt}")
      multi do
        hset("notgios.users.#{username}", 'password', crypted)
        hset("notgios.users.#{username}", 'salt', salt)
        sadd("notgios.users", username)
      end
    end

    def authenticate_user(username, pass)
      raise NoSuchResourceError, "User #{username} does not exist." unless exists("notgios.users.#{username}")
      salt = hget("notgios.users.#{username}", 'salt')
      crypted = hget("notgios.users.#{username}", 'password')
      crypted == Digest::SHA256.hexdigest("#{pass}|#{salt}")
    end

    def user_exists?(username)
      exists("notgios.users.#{username}")
    end

    # FIXME: Revisit this in the future to make sure it actually clears all user data.
    def delete_user(username)
      success = nil
      until success.exists?
        watch([
          "notgios.users.#{username}",
          "notgios.users.#{username}.servers",
          "notgios.users.#{username}.jobs",
          "notgios.users.#{username}.contacts"
        ])
        servers = smembers("notgios.users.#{username}.servers")
        jobs = smembers("notgios.users.#{username}.jobs")
        contacts = smembers("notgios.users.#{username}.contacts")
        success = multi do
          srem("notgios.users", username)
          del("notgios.users.#{username}")
          del("notgios.users.#{username}.servers")
          del("notgios.users.#{username}.jobs")
          servers.each { |server| del("notgios.servers.#{server}") }
          jobs.each do |job|
            del("notgios.jobs.#{job}")
            del("notgios.reports.#{job}")
          end
          contacts.each { |contact| del("notgios.contacts.#{contact}") }
        end
      end
    end

    # Job Commands

    # Expects:
    # username - String
    # address - String
    # job - CommandStruct
    def add_job(username, job)
      raise WrongUserError, "Server #{job.server} is not owned by user #{username}" unless sismember("notgios.users.#{username}.servers", job.server)
      job.id = incr('notgios.id')
      sadd("notgios.users.#{username}.jobs", job.id)
      job_hash = job.to_h
      job_hash.delete('command')
      job_hash[:options] = job_hash[:options].to_json
      hmset("notgios.jobs.#{job.id}", *job_hash)
    end

    # Expects:
    # username - Stirng
    # id - String, Job id
    def get_job(username, id)
      raise WrongUserError, "Job #{id} is not owned by user #{username}" unless sismember("notgios.users.#{username}.jobs", id)
      raise NoSuchResourceError unless exists("notgios.jobs.#{id}")
      job = CommandStruct.new
      hgetall("notgios.jobs.#{id}").each_pair do |key, value|
        job.send(key + '=', value)
      end
      job.options = JSON.parse(options)
      job
    rescue NoMethodError, ArgumentError
      raise InvalidJobError, "Job #{id} exists but is malformatted"
    end

    # Expects:
    # username - String
    def get_jobs_for_user(username)
      raise NoSuchResourceError unless exists("notgios.users.#{username}")
      jobs = Array.new
      smembers("notgios.users.#{username}.jobs").each do |job|
        jobs.push(hgetall("notgios.jobs.#{job}"))
      end
      jobs
    end

    # Only to be used during startup and by the Prospector.
    def get_all_jobs
      jobs = Hash.new { |h, k| h[k] = Array.new }
      smembers("notgios.users").each do |user|
        smembers("notgios.users.#{user}.jobs").each do |job|
          job_hash = hgetall("notgios.jobs.#{job}")
          command = CommandStruct.new
          job_hash.each_pair { |key, value| command.send(key + '=', value) }
          command.options = JSON.parse(command.options)
          jobs[job_hash['server']].push(command)
        end
      end
      jobs
    end

    # Expects:
    # username - String
    # job - CommandStruct
    # Will overwrite all job attributes with new ones.
    def update_job(username, job)
      raise WrongUserError, "Job #{job.id} is not owned by user #{username}" unless sismember("notgios.users.#{username}.jobs", job.id)
      raise NoSuchResourceError, "Job #{job.id} does not exist" unless exists("notgios.jobs.#{job.id}")
      job_hash = job.to_h
      job_hash.delete('command')
      hmset("notgios.jobs.#{job.id}", *job.to_h)
    end

    # Expects:
    # id - Fixnum
    # report - JSON String
    def post_job_report(id, report)
      raise NoSuchResourceError, "Job #{id} does not exist" unless exists("notgios.jobs.#{id}")
      type = hget("notgios.jobs.#{id}", 'type')
      metric = hget("notgios.jobs.#{id}", 'metric')

      # Grab the timestamp for the report.
      timestamp = report.shift.scan(/TIMESTAMP (\d+)/)
      raise InvalidJobError, 'Timestamp field of job report was malformed' unless timestamp.exists? && timestamp.first.exists?
      timestamp = timestamp.first.first

      case type.downcase
      when 'process', 'total'
        case metric.downcase
        when 'cpu'
          # Grab the CPU usage and add it to the zset.
          percent = report.shift.scan(/CPU PERCENT (\d+\.\d+)/)
          if percent.exists? && percent.first.exists?
            lpush("notgios.reports.#{id}", { cpu: percent.first.first, timestamp: timestamp.to_i }.to_json)
          else
            raise InvalidJobError, 'CPU field of job report was malformed'
          end
        when 'memory'
          # Grab the memory usage and add it to the zset.
          memory = report.shift.scan(/BYTES (\d+)/)
          if memory.exists? && memory.first.exists?
            lpush("notgios.reports.#{id}", { bytes: memory.first.first, timestamp: timestamp.to_i }.to_json)
          else
            raise InvalidJobError, 'BYTES field of job report was malformed'
          end
        when 'io'
          raise UnsupportedJobError, 'Job metric IO isn\'t supported currently'
        else
          raise InvalidJobError, "Unknown job metric #{metric} for process type"
        end
      when 'directory'
        case metric.downcase
        when 'memory'
          # Grab the memory usage and add it to the zset.
          memory = report.shift.scan(/BYTES (\d+)/)
          if memory.exists? && memory.first.exists?
            lpush("notgios.reports.#{id}", { bytes: memory.first.first, timestamp: timestamp.to_i }.to_json)
          else
            raise InvalidJobError, 'BYTES field of job report was malformed'
          end
        else
          raise InvalidJobError, "Unknown job metric #{metric} for directory type"
        end
      when 'disk'
        raise UnsupportedJobError, 'Job type disk isn\'t currently supported'
      when 'swap'
        raise UnsupportedJobError, 'Job type swap isn\' currently supported'
      when 'load'
        raise UnsupportedJobError, 'Job type load isn\'t currently supported'
      else
        raise InvalidJobError, "Unknown job type #{type}"
      end
    rescue NoMethodError
      raise InvalidJobError, 'Hit a NoMethodError while processing job'
    end

    # Expects:
    # username - String
    # job - Fixnum
    def delete_job(username, id)
      raise WrongUserError, "Job #{id} is not owned by user #{username}" unless sismember("notgios.users.#{username}.jobs", id)
      raise NoSuchResourceError, "Job #{job.id} does not exist" unless exists("notgios.jobs.#{id}")
      multi do
        srem("notgios.users.#{username}.jobs", id)
        del("notgios.jobs.#{id}")
        del("notgios.reports.#{id}")
      end
    end

    # Server Commands

    # Expects:
    # username - String
    # address - String, ip address of server
    # name - Name given to server by the user.
    # ssh_port - Fixnum
    def add_server(username, starting_job, address, name, ssh_port = 22)
      raise ResourceExistsError if exists("notgios.servers.#{address}")
      multi do
        sadd("notgios.users.#{username}.servers", address)
        hset("notgios.servers.#{address}", 'name', name)
        hset("notgios.servers.#{address}", 'address', address)
        hset("notgios.servers.#{address}", 'ssh_port', ssh_port)
        hset("notgios.servers.#{address}", 'connected', false)
      end
      add_job(username, starting_job)
    end

    def update_server(username, address, name, ssh_port)
      raise WrongUserError, "Server #{address} is not owned by used #{username}" unless sismember("notgios.users.#{username}.servers", address)
      multi do
        hset("notgios.servers.#{address}", 'name', name)
        hset("notgios.servers.#{address}", 'ssh_port', ssh_port)
      end
    end

    # This is left unauthenticated because it's only to be called from the MiddleMan.
    def mark_connected(address)
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      hset("notgios.servers.#{address}", 'connected', 'true')
    end

    # This is left unauthenticated because it's only to be called from the MiddleMan.
    def mark_disconnected(address)
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      hset("notgios.servers.#{address}", 'connected', 'false')
    end

    def server_seen(address)
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      hset("notgios.servers.#{address}", 'lastSeen', Time.now.to_i)
    end

    # Expects:
    # username - String
    # address - String, ip address of server
    def get_hostname(username, address)
      raise WrongUserError, "Server #{address} is not owned by user #{username}" unless sismember("notgios.users.#{username}.servers", address)
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      hget("notgios.servers.#{address}", 'hostname')
    end

    # Expects:
    # username - String
    # address - String, ip address of server
    def get_ssh_port(username, address)
      raise WrongUserError, "Server #{address} is not owned by user #{username}" unless sismember("notgios.users.#{username}.servers", address)
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      hget("notgios.servers.#{address}", 'ssh_port')
    end

    def get_servers(username)
      raise NoSuchResourceError, "User #{username} does not exist" unless exists("notgios.users.#{username}")
      servers = Array.new
      smembers("notgios.users.#{username}.servers").each { |address| servers.push(hgetall("notgios.servers.#{address}")) }
      servers.map do |server|
        port = server.delete('ssh_port')
        server['sshPort'] = port
        server
      end
    end

    # Expects:
    # username - String
    # address - String, ip address of server
    def delete_server(username, address)
      raise WrongUserError, "Server #{address} is not owned by user #{username}" unless sismember("notgios.users.#{username}.servers", address)
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      jobs = Array.new
      smembers("notgios.users.#{username}.jobs").each { |id| jobs.push(id) if hget("notgios.jobs.#{id}", 'server') == address }
      multi do
        srem("notgios.users.#{username}.servers", address)
        del("notgios.servers.#{address}")
        jobs.each do |job|
          srem("notgios.users.#{username}.jobs", job)
          del("notgios.jobs.#{job}")
        end
      end
    end

    # Contact Commands

    def add_contact(username, method, value)
      contact_id = incr("notgios.contacts.id")
      multi do
        sadd("notgios.users.#{username}.contacts", contact_id)
        hmset("notgios.contacts.#{contact_id}", 'method', method, 'value', value)
      end
    end

    def get_contacts(username)
      raise NoSuchResourceError, "User #{username} does not exist" unless exists("notgios.users.#{username}")
      contacts = Array.new
      smembers("notgios.users.#{username}.contacts").each { |id| contacts.push(hgetall("notgios.contacts.#{id}")) }
      contacts
    end

    # Metrics Commands

    # Expects
    # id - Fixnum, job id
    # count - Maximum number of results to return.
    def get_recent_metrics(id, username = nil, count = 100)
      raise WrongUserError, "Job #{id} does not belong to user #{username}" unless username.nil? || sismember("notgios.users.#{username}.jobs", id)
      raise NoSuchResourceError, "Job #{id} does not exist" unless exists("notgios.jobs.#{id}")
      lrange("notgios.reports.#{id}", 0, count).map do |resp|
        report = JSON.parse(resp.first)
        report['timestamp'] = resp.last
        report
      end
    end

    # Helpers

    def next_id
      incr('notgios.id')
    end

  end
end
