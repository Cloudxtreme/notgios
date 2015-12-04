module Notgios

  class Nodis < Redis

    ResourceExistsError = Class.new(StandardError)
    NoSuchResourceError = Class.new(StandardError)
    InvalidJobError = Class.new(StandardError)
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
    # job - CommandStruct
    def add_job(username, job)
      job.id = incr('notgios.id')
      sadd("notgios.users.#{username}.jobs", job.id)
      hmset("notgios.jobs.#{job.id}", *job.to_h.delete('command'))
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
      job
    rescue NoMethodError, ArgumentError
      raise InvalidJobError, "Job #{id} exists but is malformatted"
    end

    # Only to be used during startup.
    def get_all_jobs
      jobs = Hash.new { |h, k| h[k] = Array.new }
      smembers("notgios.users").each do |user|
        smembers("notgios.users.#{user}.jobs").each do |job|
          job_hash = hgetall("notgios.jobs.#{job}")
          command = CommandStruct.new
          job_hash.each_pair { |key, value| command.send(key + '=', value) }
          jobs[job_hash['host']].push(command)
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
      hmset("notgios.jobs.#{job.id}", *job.to_h.delete('command'))
    end

    # Expects:
    # id - Fixnum
    # report - JSON String
    def post_job_report(id, report)
      raise NoSuchResourceError, "Job #{id} does not exist" unless exists("notgios.jobs.#{id}")
      lpush("notgios.reports.#{id}", report)
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
    # hostname - String, hostname of server if one exists, otherwise ip address again
    # ssh_port - Fixnum
    def add_server(username, address, hostname, ssh_port = 22)
      raise ResourceExistsError if exists("notgios.servers.#{address}")
      sadd("notgios.users.#{username}.servers", address)
      hset("notgios.servers.#{address}", 'hostname', hostname)
      hset("notgios.servers.#{address}", 'ssh_port', ssh_port)
    end

    # Expects:
    # username - String
    # address - String, ip address of server
    def get_hostname(username, address)
      raise WrongUserError, "Server #{address} is not owned by user #{username}" unless sismember("notgios.users.#{username}.servers")
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      hget("notgios.servers.#{address}", 'hostname')
    end

    # Expects:
    # username - String
    # address - String, ip address of server
    def get_ssh_port(username, address)
      raise WrongUserError, "Server #{address} is not owned by user #{username}" unless sismember("notgios.users.#{username}.servers")
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      hget("notgios.servers.#{address}", 'ssh_port')
    end

    def get_servers(username)
      raise NoSuchResourceError, "User #{username} does not exist" unless exists("notgios.users.#{username}")
      servers = Array.new
      smembers("notgios.users.#{username}.servers").each { |address| servers.push(hgetall("notgios.servers.#{address}")) }
      servers
    end

    # Expects:
    # username - String
    # address - String, ip address of server
    def delete_server(username, address)
      raise WrongUserError, "Server #{address} is not owned by user #{username}" unless sismember("notgios.users.#{username}.servers")
      raise NoSuchResourceError, "Server #{address} does not exist" unless exists("notgios.servers.#{address}")
      multi do
        srem("notgios.users.#{username}.servers", address)
        del("notgios.servers.#{address}")
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

  end
end
